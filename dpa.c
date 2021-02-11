#include "dpa.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#define PATH(arg)           (arg)->ts_path
#define CALC_THRD(arg)      (arg)->n_thrd
#define N_PMS(arg)          (arg)->n_power_models

struct result_data
{
    sem_t lock;
    size_t num_hyp, num_samples, num_traces;
    size_t num_calculated;

    double **pm_sum, **pm_sq;
    double *tr_sum, *tr_sq;
    double **product;
};

struct thread_arg
{
    int index;
    struct dpa_args *dpa_arg;

    enum thread_cmd
    {
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
    struct result_data *local_res;
    size_t base, tpb, curr;
};

#define LINEAR(res, pi, ti)     ((res)->num_hyp * (ti) + (pi))

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
                             2 * n_pm * 256 * ts_num_samples(set));
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
    trace_data_output(t, &data);
    val1 = data[target];
    val2 = data[(target + 4 * (target % 4)) % 16];

//    trace_data_all(t, &data);
//    val1 = data[16 + target];
//    val2 = data[16 + (target + 4 * (target % 4)) % 16];

    state = val1 ^ ((uint8_t) guess);
    state = sbox_inv[state >> 4u][state & 0xfu];
    return (float) hamming_distance(state, val2);
}

void *calc_thread_func(void *arg)
{
    struct thread_arg *my_arg = (struct thread_arg *) arg;
    struct trace *curr;
    size_t trace;

    int ret, k, i, p;
    float *trace_data, power_models[N_PMS(my_arg->dpa_arg)][256];
    struct timeval start, stop, diff;
    struct result_data *local_res = my_arg->local_res;

    gettimeofday(&start, NULL);
    for(trace = 0;
        trace < my_arg->tpb &&
        my_arg->base + trace < ts_num_traces(my_arg->set);
        trace++)
    {
        gettimeofday(&stop, NULL);
        timersub(&stop, &start, &diff);

        if(diff.tv_sec >= 1)
        {
            my_arg->curr = trace;
            gettimeofday(&start, NULL);
        }

        ret = trace_get(my_arg->set, &curr, my_arg->base + trace, false);
        if(ret < 0)
        {
            my_arg->status = ret;
            break;
        }

        ret = trace_samples(curr, &trace_data);
        if(ret < 0)
        {
            my_arg->status = ret;
            break;
        }

        sem_wait(&local_res->lock);

        local_res->num_calculated++;
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
                        power_models[p][k] = power_model(curr, k, p);
                        local_res->pm_sum[p][k] += power_models[p][k];
                        local_res->pm_sq[p][k] += (power_models[p][k] * power_models[p][k]);
                    }

                    local_res->product[p][LINEAR(local_res, k, i)] +=
                            power_models[p][k] * trace_data[i];
                }
            }
        }
        sem_post(&local_res->lock);

        trace_free(curr);
    }

    my_arg->curr = trace;
    my_arg->status = STAT_DONE;
}

#define RESET_TIMERS(now, wait)             \
       gettimeofday(&(now), NULL);          \
       (wait).tv_sec = (now).tv_sec + 1;    \
       (wait).tv_nsec = 0

// todo: unglobal this
double *global_max_pearson, *global_max_k, *global_max_i;
size_t last_nproc;

void print_progress(struct thread_arg *t_args,
                    struct result_data *res,
                    struct dpa_args *dpa_arg)
{
    int i, j, k, p;
    double done;

    double pearson, pm_avg, pm_dev, tr_avg, tr_dev;
    double max_pearson, max_k, max_i;

    if(!global_max_pearson)
        global_max_pearson = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_k)
        global_max_k = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_i)
        global_max_i = calloc(N_PMS(dpa_arg), sizeof(double));

    for(j = 0; j < CALC_THRD(dpa_arg); j++)
    {
        sem_wait(&t_args[j].local_res->lock);

        if(j == 0)
        {
            res->num_calculated = t_args[j].local_res->num_calculated;
            for(i = 0; i < ts_num_samples(t_args[j].set); i++)
            {
                res->tr_sum[i] = t_args[j].local_res->tr_sum[i];
                res->tr_sq[i] = t_args[j].local_res->tr_sq[i];

                for(p = 0; p < N_PMS(dpa_arg); p++)
                {
                    for(k = 0; k < 256; k++)
                    {
                        if(i == 0)
                        {
                            res->pm_sum[p][k] = t_args[j].local_res->pm_sum[p][k];
                            res->pm_sq[p][k] = t_args[j].local_res->pm_sq[p][k];
                        }

                        res->product[p][LINEAR(res, k, i)] =
                                t_args[j].local_res->product[p][LINEAR(res, k, i)];
                    }
                }
            }
        }
        else
        {
            res->num_calculated += t_args[j].local_res->num_calculated;
            for(i = 0; i < ts_num_samples(t_args[j].set); i++)
            {
                res->tr_sum[i] += t_args[j].local_res->tr_sum[i];
                res->tr_sq[i] += t_args[j].local_res->tr_sq[i];

                for(p = 0; p < N_PMS(dpa_arg); p++)
                {
                    for(k = 0; k < 256; k++)
                    {
                        if(i == 0)
                        {
                            res->pm_sum[p][k] += t_args[j].local_res->pm_sum[p][k];
                            res->pm_sq[p][k] += t_args[j].local_res->pm_sq[p][k];
                        }

                        res->product[p][LINEAR(res, k, i)] +=
                                t_args[j].local_res->product[p][LINEAR(res, k, i)];
                    }
                }
            }
        }

        sem_post(&t_args[j].local_res->lock);
    }

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

    printf("\nCompute threads\n");
    for(i = 0; i < CALC_THRD(dpa_arg); i++)
    {
        done = t_args[i].tpb == 0 ?
               0 :
               ((double) t_args[i].curr /
                (double) t_args[i].tpb);

        printf("calc%i [", i);
        for(j = 0; j < 80; j++)
        {
            if((double) j / 80.0 < done)
                printf("=");
            else
                printf(" ");
        }
        printf("] %.2f%% (%li)\n", done * 100, t_args[i].base);
    }

    printf("\nCurrent Pearson (%li traces)\n", res->num_calculated);
    for(p = 0; p < N_PMS(dpa_arg); p++)
    {
        printf("\t%i %f for guess 0x%02X at sample %i\n",
               p, global_max_pearson[p], (uint8_t) global_max_k[p], (int) global_max_i[p]);
    }

    printf("\n\n\n\n");
}

bool __any_running(struct thread_arg *calc_args,
                   struct dpa_args *dpa_args)
{
    int i;
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
    size_t batch = 0, trace_per_thread;

    struct trace_set *set;
    struct result_data *res;

    pthread_t t_handles[CALC_THRD(arg)];
    struct thread_arg t_args[CALC_THRD(arg)];

    ret = ts_open(&set, PATH(arg));
    if(ret < 0)
    {
        printf("couldn't create trace set: %s\n", strerror(-ret));
        return ret;
    }

    trace_per_thread = ts_num_traces(set) / CALC_THRD(arg);

    if(__init_result_data(&res, set, arg) < 0)
    {
        printf("couldn't init result data\n");
        goto __close_set;
    }

    for(i = 0; i < CALC_THRD(arg); i++)
    {
        // create calculation threads
        t_args[i].index = i;
        t_args[i].dpa_arg = arg;
        t_args[i].status = STAT_RUNNING;
        t_args[i].set = set;

        __init_result_data(&t_args[i].local_res, set, arg);
        t_args[i].base = i * trace_per_thread;
        t_args[i].tpb = trace_per_thread;
        t_args[i].curr = 0;

        ret = pthread_create(&t_handles[i], NULL,
                             calc_thread_func, &t_args[i]);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_calc_args;
        }
    }

    while(__any_running(t_args, arg))
    {
        usleep(1000000);
        print_progress(t_args, res, arg);
    }

    print_progress(t_args, res, arg);

    i = CALC_THRD(arg) - 1;
__free_calc_args:
    for(j = 0; j <= i; j++)
    {
        // todo figure out some cancellation mechanism
        pthread_join(t_handles[i], NULL);
//        sem_destroy(&t_args[i].start);
    }

    if(res)
        __free_result_data(res, arg);

__close_set:
    ts_close(set);
    return ret;
}
