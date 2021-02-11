#include "dpa.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#define PATH(arg)           (arg)->ts_path
#define MAX_RAM(arg)         (arg)->max_ram
#define BUF_THRD(arg)       (arg)->n_buf_thrd
#define CALC_THRD(arg)      (arg)->n_calc_thrd
#define ALL_THRD(arg)       BUF_THRD(arg) + CALC_THRD(arg)
#define N_PMS(arg)          (arg)->n_power_models

struct thread_arg
{
    int index;
    sem_t start, *global_done;
    struct dpa_args *dpa_arg;

    enum thread_cmd
    {
        // buffering
        CMD_GET = 0,

        // calculation
        CMD_CALC,

        // generic
        CMD_NOP,
        CMD_EXIT
    } cmd;

    enum thread_stat
    {
        STAT_READY = 0,
        STAT_RUNNING,
        STAT_DONE,
        STAT_FAILED
    } status;

    struct trace_set *set;
    struct trace **t;
    void *other_data;
    size_t base, tpb, curr;
};

struct result_data
{
    sem_t lock;
    size_t num_hyp, num_samples, num_traces;
    size_t num_calculated;
    size_t num_new;

    double **pm_sum, **pm_sq;
    double *tr_sum, *tr_sq;
    double **product;
};

#define LINEAR(res, pi, ti)     ((res)->num_hyp * (ti) + (pi))

void *buffer_thread_func(void *arg)
{
    struct thread_arg *my_arg = (struct thread_arg *) arg;
    size_t trace;
    int ret;

    struct timeval start, stop, diff;

    while(1)
    {
        sem_wait(&my_arg->start);
        switch(my_arg->cmd)
        {
            case CMD_EXIT:
                return NULL;

            case CMD_NOP:
                break;

            case CMD_GET:
                gettimeofday(&start, NULL);

                for(trace = 0;
                    trace < my_arg->tpb &&
                    my_arg->base + trace < ts_num_traces(my_arg->set);
                    trace++)
                {
                    gettimeofday(&stop, NULL);
                    timersub(&stop, &start, &diff);

                    if(diff.tv_usec >= 20000)
                    {
                        gettimeofday(&start, NULL);
                        my_arg->curr = trace;
                    }

                    ret = trace_get(my_arg->set,
                                    &my_arg->t[trace],
                                    my_arg->base + trace,
                                    true);
                    if(ret < 0)
                    {
                        my_arg->status = ret;
                        break;
                    }
                }

                my_arg->curr = trace;
                my_arg->status = STAT_DONE;
                //printf("buffer %i done buffering base %li\n", my_arg->index, my_arg->base);
                break;

            case CMD_CALC:
                my_arg->status = STAT_FAILED;
                break;
        }

        sem_post(my_arg->global_done);
    }
}

void __free_result_data(struct result_data *data,
                        struct dpa_args *dpa_arg)
{
    int i;

    sem_destroy(&data->lock);
    for(i = 0; i < N_PMS(dpa_arg); i++)
    {
        if(data->pm_sum && data->pm_sum[i])
            free(data->pm_sum[i]);

        if(data->pm_sq && data->pm_sq[i])
            free(data->pm_sq[i]);

        if(data->product && data->product[i])
            free(data->product[i]);
    }

    if(data->pm_sum)
        free(data->pm_sum);

    if(data->pm_sq)
        free(data->pm_sq);

    if(data->product)
        free(data->product);

    if(data->tr_sum)
        free(data->tr_sum);

    if(data->tr_sq)
        free(data->tr_sq);

    free(data);
}

int __init_result_data(struct result_data **data,
                       struct trace_set *set,
                       struct dpa_args *dpa_arg)
{
    int ret, i;
    struct result_data *res;
    res = calloc(1, sizeof(struct result_data));

    if(!res)
        return -ENOMEM;

    res->num_hyp = 256;
    res->num_samples = ts_num_samples(set);
    res->num_traces = ts_num_traces(set);
    res->num_calculated = 0;

    ret = sem_init(&res->lock, 0, 1);
    if(ret < 0)
    {
        free(res);
        return -errno;
    }

    res->pm_sum = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->pm_sum)
        goto __fail;

    res->pm_sq = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->pm_sq)
        goto __fail;

    res->product = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->product)
        goto __fail;

    for(i = 0; i < N_PMS(dpa_arg); i++)
    {
        res->pm_sum[i] = calloc(256, sizeof(double));
        if(!res->pm_sum[i])
            goto __fail;

        res->pm_sq[i] = calloc(256, sizeof(double));
        if(!res->pm_sq[i])
            goto __fail;

        res->product[i] = calloc(256 * ts_num_samples(set), sizeof(double));
        if(!res->product[i])
            goto __fail;
    }

    res->tr_sum = calloc(ts_num_samples(set), sizeof(double));
    if(!res->tr_sum)
        goto __fail;

    res->tr_sq = calloc(ts_num_samples(set), sizeof(double));
    if(!res->tr_sq)
        goto __fail;

    *data = res;
    return 0;

__fail:
    __free_result_data(res, dpa_arg);
    *data = NULL;
    return -ENOMEM;
}

size_t __result_data_size(struct trace_set *set, int n_pm)
{
    return sizeof(double) * (n_pm * 2 * 256 +
                             2 * ts_num_samples(set) +
                             n_pm * 256 * ts_num_samples(set));
}

static const unsigned char sbox_inv[16][16] =
        {
                {0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb},
                {0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb},
                {0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e},
                {0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25},
                {0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92},
                {0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84},
                {0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06},
                {0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b},
                {0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73},
                {0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e},
                {0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b},
                {0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4},
                {0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f},
                {0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef},
                {0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61},
                {0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d}
        };

static inline int hamming_weight(unsigned char n)
{
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);

    return (char) n;
}

static inline int hamming_distance(unsigned char n, unsigned char p)
{
    return hamming_weight(n ^ p);
}

static inline float power_model(struct trace *t, size_t guess, int target)
{
    uint8_t *data, state, val1, val2;
//    trace_data_output(t, &data);

    trace_data_all(t, &data);

    val1 = data[16 + target];
    val2 = data[16 + (target + 4 * (target % 4)) % 16];

    state = val1 ^ ((uint8_t) guess);
    state = sbox_inv[state >> 4u][state & 0xfu];
    return (float) hamming_distance(state, val2);
}

void *calc_thread_func(void *arg)
{
    struct thread_arg *my_arg = (struct thread_arg *) arg;
    struct result_data *res = (struct result_data *) my_arg->other_data;

    size_t trace, last_trace;

    int ret, k, i, p;
    float *trace_data, power_models[N_PMS(my_arg->dpa_arg)][256];
    struct timeval start, stop, diff;
    struct result_data *local_res;

    // todo cleanly fail
    __init_result_data(&local_res, my_arg->set, my_arg->dpa_arg);

    while(1)
    {
        sem_wait(&my_arg->start);
        switch(my_arg->cmd)
        {
            case CMD_EXIT:
                return NULL;

            case CMD_NOP:
                break;

            case CMD_CALC:
                gettimeofday(&start, NULL);
                last_trace = 0;

                for(trace = 0;
                    trace < my_arg->tpb &&
                    my_arg->base + trace < ts_num_traces(my_arg->set);
                    trace++)
                {
                    gettimeofday(&stop, NULL);
                    timersub(&stop, &start, &diff);

                    if(diff.tv_sec >= 1)
                    {
                        if(sem_trywait(&res->lock) == 0)
                        {
                            res->num_calculated += (trace - last_trace);
                            last_trace = trace;

                            for(i = 0; i < ts_num_samples(my_arg->set); i++)
                            {
                                res->tr_sum[i] += local_res->tr_sum[i];
                                res->tr_sq[i] += local_res->tr_sq[i];

                                for(p = 0; p < N_PMS(my_arg->dpa_arg); p++)
                                {
                                    for(k = 0; k < 256; k++)
                                    {
                                        if(i == 0)
                                        {
                                            res->pm_sum[p][k] += local_res->pm_sum[p][k];
                                            res->pm_sq[p][k] += local_res->pm_sq[p][k];
                                        }

                                        res->product[p][LINEAR(res, k, i)] +=
                                                local_res->product[p][LINEAR(res, k, i)];
                                    }
                                }
                            }
                            sem_post(&res->lock);


                            for(p = 0; p < N_PMS(my_arg->dpa_arg); p++)
                            {
                                memset(local_res->pm_sum[p], 0, 256 * sizeof(double));
                                memset(local_res->pm_sq[p], 0, 256 * sizeof(double));
                                memset(local_res->product[p], 0, 256 * ts_num_samples(my_arg->set) * sizeof(double));
                            }

                            memset(local_res->tr_sum, 0, ts_num_samples(my_arg->set) * sizeof(double));
                            memset(local_res->tr_sq, 0, ts_num_samples(my_arg->set) * sizeof(double));
                        }

                        my_arg->curr = trace;
                        gettimeofday(&start, NULL);
                    }

                    ret = trace_samples(my_arg->t[trace], &trace_data);
                    if(ret < 0)
                    {
                        my_arg->status = ret;
                        break;
                    }

                    for(i = 0; i < ts_num_samples(my_arg->set); i++)
                    {
                        local_res->tr_sum[i] += trace_data[i];
                        local_res->tr_sq[i] += (trace_data[i] * trace_data[i]);

                        for(p = 0; p < N_PMS(my_arg->dpa_arg); p++)
                        {
                            for(k = 0; k < 256; k++)
                            {
                                if(i == 0)
                                {
                                    power_models[p][k] = power_model(my_arg->t[trace], k, p);
                                    local_res->pm_sum[p][k] += power_models[p][k];
                                    local_res->pm_sq[p][k] += (power_models[p][k] * power_models[p][k]);
                                }

                                local_res->product[p][LINEAR(res, k, i)] +=
                                        power_models[p][k] * trace_data[i];
                            }
                        }
                    }
                }

                my_arg->status = STAT_DONE;
                break;

            case CMD_GET:
                my_arg->status = STAT_FAILED;
                break;
        }

        sem_post(my_arg->global_done);
    }
}

#define RESET_TIMERS(now, wait)             \
       gettimeofday(&(now), NULL);          \
       (wait).tv_sec = (now).tv_sec + 1;    \
       (wait).tv_nsec = 0

// todo: unglobal this
double *global_max_pearson, *global_max_k, *global_max_i;
size_t last_nproc;

void print_progress(struct thread_arg *buffer_args,
                    struct thread_arg *calc_args,
                    struct result_data *res,
                    struct dpa_args *dpa_arg)
{
    int i, j, k, p;
    size_t nproc;
    double done;

    double pearson, pm_avg, pm_dev, tr_avg, tr_dev;
    double max_pearson, max_k, max_i;

    if(!global_max_pearson)
        global_max_pearson = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_k)
        global_max_k = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_i)
        global_max_i = calloc(N_PMS(dpa_arg), sizeof(double));

    sem_wait(&res->lock);
    nproc = res->num_calculated;

    if(nproc - last_nproc > 250)
    {
    for(p = 0; p < N_PMS(dpa_arg); p++)
    {
        max_pearson = 0;

        for(k = 0; k < 256; k++)
        {
            for(i = 0; i < res->num_samples; i++)
            {
                pm_avg = res->pm_sum[p][k] / res->num_calculated;
                pm_dev = res->pm_sq[p][k] / res->num_calculated;
                pm_dev -= (pm_avg * pm_avg);
                pm_dev = sqrt(pm_dev);

                tr_avg = res->tr_sum[i] / res->num_calculated;
                tr_dev = res->tr_sq[i] / res->num_calculated;
                tr_dev -= (tr_avg * tr_avg);
                tr_dev = sqrt(tr_dev);

                pearson = res->product[p][LINEAR(res, k, i)];
                pearson -= res->tr_sum[i] * pm_avg;
                pearson -= res->pm_sum[p][k] * tr_avg;
                pearson += res->num_calculated * pm_avg * tr_avg;

                pearson /= (res->num_calculated * pm_dev * tr_dev);
                if(fabs(pearson) > max_pearson)
                {
                    max_pearson = fabs(pearson);
                    max_k = k;
                    max_i = i;
                }
            }
        }

        global_max_pearson[p] = max_pearson;
        global_max_k[p] = max_k;
        global_max_i[p] = max_i;
    }
    last_nproc = nproc;
    }
    sem_post(&res->lock);

    printf("Buffer threads\n");
    for(i = 0; i < BUF_THRD(dpa_arg); i++)
    {
        done = buffer_args[i].tpb == 0 ?
               0 :
               ((double) buffer_args[i].curr /
                (double) buffer_args[i].tpb);

        printf("buf%i [", i);
        for(j = 0; j < 80; j++)
        {
            if((double) j / 80.0 < done)
                printf("=");
            else
                printf(" ");
        }
        printf("] %.2f%% (%li)\n", done * 100, buffer_args[i].base);
    }

    printf("\nCompute threads\n");
    for(i = 0; i < CALC_THRD(dpa_arg); i++)
    {
        done = calc_args[i].tpb == 0 ?
               0 :
               ((double) calc_args[i].curr /
                (double) calc_args[i].tpb);

        printf("calc%i [", i);
        for(j = 0; j < 80; j++)
        {
            if((double) j / 80.0 < done)
                printf("=");
            else
                printf(" ");
        }
        printf("] %.2f%% (%li)\n", done * 100, calc_args[i].base);
    }

    printf("\nCurrent Pearson (%li traces)\n", nproc);
    for(p = 0; p < N_PMS(dpa_arg); p++)
    {
        printf("\t%i %f for guess 0x%02X at sample %i\n",
               p, global_max_pearson[p], (uint8_t) global_max_k[p], (int) global_max_i[p]);
    }

    printf("\n\n\n\n");
}

bool __any_running(struct thread_arg *buffer_args,
                   struct thread_arg *calc_args,
                   struct dpa_args *dpa_args)
{
    int i;
    for(i = 0; i < BUF_THRD(dpa_args); i++)
    {
        if(buffer_args[i].status == STAT_RUNNING)
            return true;
    }

    for(i = 0; i < CALC_THRD(dpa_args); i++)
    {
        if(calc_args[i].status == STAT_RUNNING)
            return true;
    }

    return false;
}

int run_dpa(struct dpa_args *arg)
{
    int i, j, k, ret;
    size_t trace_per_block, batch = 0;

    struct trace_set *set;
    struct trace **trace_blocks[ALL_THRD(arg)];
    enum trace_status
    {
        EMPTY = 0, FETCH, CALC
    } trace_status[ALL_THRD(arg)];
    int trace_index[ALL_THRD(arg)];

    struct result_data *res;

    sem_t global_done;
    bool sem_init_success;
    pthread_t buffer_threads[BUF_THRD(arg)];
    pthread_t calc_threads[CALC_THRD(arg)];
    struct thread_arg buffer_args[BUF_THRD(arg)];
    struct thread_arg calc_args[CALC_THRD(arg)];

    ret = ts_open(&set, PATH(arg));
    if(ret < 0)
    {
        printf("couldn't create trace set: %s\n", strerror(-ret));
        return ret;
    }

    if(MAX_RAM(arg) -
       (CALC_THRD(arg) + 1) *
       __result_data_size(set, N_PMS(arg)) < 0)
    {
        printf("not enough RAM for necessary structures\n");
        goto __close_set;
    }

    trace_per_block = ((MAX_RAM(arg) -
                        (CALC_THRD(arg) + 1) *
                        __result_data_size(set, N_PMS(arg))) /
                       ts_trace_size(set)) / (ALL_THRD(arg));

    if(trace_per_block > (ts_num_traces(set) / (ALL_THRD(arg))))
        trace_per_block = (ts_num_traces(set) / (ALL_THRD(arg)));

    if(__init_result_data(&res, set, arg) < 0)
    {
        printf("couldn't init result data\n");
        goto __close_set;
    }

    for(i = 0; i < ALL_THRD(arg); i++)
    {
        trace_status[i] = EMPTY;
        trace_blocks[i] = calloc(trace_per_block,
                                 sizeof(struct trace *));
        if(!trace_blocks[i])
            goto __free_trace_sets;
    }

    ret = sem_init(&global_done, 0, BUF_THRD(arg));
    if(ret < 0)
    {
        ret = errno;
        goto __free_trace_sets;
    }

    for(i = 0; i < BUF_THRD(arg); i++)
    {
        // create buffering threads
        buffer_args[i].index = i;
        buffer_args[i].dpa_arg = arg;
        buffer_args[i].status = STAT_READY;
        buffer_args[i].set = set;
        buffer_args[i].t = NULL;
        buffer_args[i].other_data = NULL;
        buffer_args[i].base = 0;
        buffer_args[i].tpb = trace_per_block;
        buffer_args[i].curr = 0;
        buffer_args[i].global_done = &global_done;

        sem_init_success = false;
        ret = sem_init(&buffer_args[i].start, 0, 0);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_buffer_threads;
        }

        sem_init_success = true;
        ret = pthread_create(&buffer_threads[i], NULL,
                             buffer_thread_func, &buffer_args[i]);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_buffer_threads;
        }
    }

    for(i = 0; i < CALC_THRD(arg); i++)
    {
        // create calculation threads
        calc_args[i].index = i;
        calc_args[i].dpa_arg = arg;
        calc_args[i].status = STAT_READY;
        calc_args[i].set = set;
        calc_args[i].t = NULL;
        calc_args[i].other_data = res;
        calc_args[i].base = 0;
        calc_args[i].tpb = trace_per_block;
        calc_args[i].curr = 0;
        calc_args[i].global_done = &global_done;

        sem_init_success = false;
        ret = sem_init(&calc_args[i].start, 0, 0);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_calc_args;
        }

        sem_init_success = true;
        ret = pthread_create(&calc_threads[i], NULL,
                             calc_thread_func, &calc_args[i]);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_calc_args;
        }
    }

    struct timeval now;
    struct timespec wait;

    while(batch < ts_num_traces(set) ||
          __any_running(buffer_args, calc_args, arg))
    {
        RESET_TIMERS(now, wait);
        while(sem_timedwait(&global_done, &wait) < 0)
        {
            print_progress(buffer_args, calc_args, res, arg);
            RESET_TIMERS(now, wait);
        }

        for(i = 0; i < ALL_THRD(arg); i++)
        {
            switch(trace_status[i])
            {
                case CALC:
                    if(calc_args[trace_index[i]].status == STAT_DONE)
                    {
                        for(k = 0; k < trace_per_block; k++)
                        {
                            trace_free(calc_args[trace_index[i]].t[k]);
                            calc_args[trace_index[i]].t[k] = NULL;
                        }

                        calc_args[trace_index[i]].status = STAT_READY;
                        calc_args[trace_index[i]].t = NULL;
                        calc_args[trace_index[i]].base = 0;
                        calc_args[trace_index[i]].tpb = 0;
                        calc_args[trace_index[i]].curr = 0;

                        trace_status[i] = EMPTY;
                        trace_index[i] = -1;
                        sem_post(&global_done);
                    }
                    break;

                case FETCH:
                    if(buffer_args[trace_index[i]].status == STAT_DONE)
                    {
                        // search for an empty calc thread
                        for(j = 0; j < CALC_THRD(arg); j++)
                        {
                            if(calc_args[j].status == STAT_READY)
                            {
                                calc_args[j].status = STAT_RUNNING;
                                calc_args[j].t = buffer_args[trace_index[i]].t;
                                calc_args[j].cmd = CMD_CALC;
                                calc_args[j].base = buffer_args[trace_index[i]].base;
                                calc_args[j].tpb = buffer_args[trace_index[i]].tpb;
                                calc_args[j].curr = 0;
                                sem_post(&calc_args[j].start);

                                buffer_args[trace_index[i]].status = STAT_READY;
                                buffer_args[trace_index[i]].t = NULL;
                                buffer_args[trace_index[i]].base = 0;
                                buffer_args[trace_index[i]].tpb = 0;
                                buffer_args[trace_index[i]].curr = 0;

                                trace_status[i] = CALC;
                                trace_index[i] = j;
                                break;
                            }
                        }
                    }
                    break;

                case EMPTY:
                    for(j = 0; j < BUF_THRD(arg) &&
                               batch < ts_num_traces(set); j++)
                    {
                        if(buffer_args[j].status == STAT_READY)
                        {
                            buffer_args[j].status = STAT_RUNNING;
                            buffer_args[j].t = trace_blocks[i];
                            buffer_args[j].cmd = CMD_GET;
                            buffer_args[j].base = batch;
                            buffer_args[j].tpb = (batch + trace_per_block >= ts_num_traces(set)) ?
                                                 ts_num_traces(set) - batch :
                                                 trace_per_block;
                            buffer_args[j].curr = 0;
                            sem_post(&buffer_args[j].start);

                            trace_status[i] = FETCH;
                            trace_index[i] = j;

                            batch += trace_per_block;
                            break;
                        }
                    }
                    break;
            }
        }
    }

    print_progress(buffer_args, calc_args, res, arg);

    i = CALC_THRD(arg) - 1;
__free_calc_args:
    for(j = 0; j <= i; j++)
    {
        if(j == i && i != CALC_THRD(arg) - 1)
        {
            // pthread init must've failed
            // if sem_init failed, nothing to be done here
            if(sem_init_success)
                sem_destroy(&calc_args[i].start);
        }
        else
        {
            calc_args[i].cmd = CMD_EXIT;
            sem_post(&calc_args[i].start);

            pthread_join(calc_threads[i], NULL);
            sem_destroy(&calc_args[i].start);
        }
    }

    i = BUF_THRD(arg) - 1;
__free_buffer_threads:
    for(j = 0; j <= i; j++)
    {
        if(j == i && i != BUF_THRD(arg) - 1)
        {
            // pthread init must've failed
            // if sem_init failed, nothing to be done here
            if(sem_init_success)
                sem_destroy(&buffer_args[i].start);
        }
        else
        {
            buffer_args[i].cmd = CMD_EXIT;
            sem_post(&buffer_args[i].start);

            pthread_join(buffer_threads[i], NULL);
            sem_destroy(&buffer_args[i].start);
        }
    }

__destroy_global_done:
    sem_destroy(&global_done);

__free_trace_sets:
    for(i = 0; i < ALL_THRD(arg); i++)
    {
        if(trace_blocks[i])
        {
            for(j = 0; j < trace_per_block; j++)
            {
                if(trace_blocks[i][j])
                {
                    trace_free(trace_blocks[i][j]);
                    trace_blocks[i][j] = NULL;
                }
            }

            free(trace_blocks[i]);
            trace_blocks[i] = NULL;
        }
    }

    if(res)
        __free_result_data(res, arg);

__close_set:
    ts_close(set);
    return ret;
}
