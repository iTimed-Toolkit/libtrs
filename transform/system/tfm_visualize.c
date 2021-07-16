#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"
#include "platform.h"
#include "list.h"

#include <errno.h>
#include <string.h>

#define TFM_DATA(tfm)   ((struct viz_args *) (tfm)->data)
#define GNUPLOT_CMD     "gnuplot"

#define IS_MULTIPLOT(tfm)   ((tfm)->rows != 1 || (tfm)->cols != 1)
#define NUMBER_TRACES(tfm)  ((tfm)->rows * (tfm)->cols * (tfm)->plots)

struct __tfm_viz_entry
{
    struct list_head list;
    size_t base, count;

    struct trace **traces;
};

struct tfm_visualize_data
{
    int status;
    struct viz_args *viz_args;

    LT_SEM_TYPE lock, signal;
    LT_THREAD_TYPE handle;

    size_t currently_displayed;
    struct list_head list;
};

int __plot_indices(struct viz_args *tfm, int r, int c, int p)
{
    if(tfm->order[0] == PLOTS)
    {
        if(tfm->order[1] == ROWS)
            return r * (tfm->cols * tfm->plots) + c * tfm->plots + p;
        else if(tfm->order[1] == COLS)
            return c * (tfm->rows * tfm->plots) + r * tfm->plots + p;
    }
    else if(tfm->order[0] == ROWS)
    {
        if(tfm->order[1] == PLOTS)
            return r * (tfm->cols * tfm->plots) + c + (tfm->cols) * p;
        else if(tfm->order[1] == COLS)
            return r + c + (tfm->rows * tfm->cols) * p;
    }
    else if(tfm->order[0] == COLS)
    {
        if(tfm->order[1] == ROWS)
            return c * tfm->rows + r + (tfm->rows * tfm->cols) * p;
        else if(tfm->order[1] == PLOTS)
            return c * (tfm->rows * tfm->plots) + r + (tfm->rows) * p;
    }

    return -1;
}

LT_THREAD_FUNC(__draw_gnuplot_thread, arg)
{
    bool found;
    int ret, r, c, p, s;
    int index;

    FILE *gnuplot;

    struct trace_set *ts = arg;
    struct tfm_visualize_data *tfm_arg = ts->tfm_state;
    struct viz_args *viz_args = tfm_arg->viz_args;

    struct __tfm_viz_entry *curr;

#if defined(LIBTRACE_PLATFORM_LINUX)
    // setup
    gnuplot = popen(GNUPLOT_CMD, "w");
    if(!gnuplot)
    {
        err("Failed to open gnuplot -- installed?\n");
        tfm_arg->status = -errno;
        return NULL;
    }

    if(viz_args->filename)
    {
        fprintf(gnuplot, "set term gif animate giant size 2560,1440\n");
        fprintf(gnuplot, "set output \"%s\"\n", viz_args->filename);
    }
    else fprintf(gnuplot, "set term x11\n");
    fprintf(gnuplot, "set grid\n");
    if(viz_args->samples == 0)
    {
        fprintf(gnuplot, "set samples %li\n", ts_num_samples(ts));
        viz_args->samples = ts_num_samples(ts);
    }
    else
    {
        if(ts_num_samples(ts) % viz_args->samples != 0)
        {
            err("Trace samples not evenly divisible by display size\n");
            ret = -EINVAL;
            goto __done;
        }

        fprintf(gnuplot, "set samples %i\n", viz_args->samples);
    }
//    fprintf(gnuplot, "unset key\n");

    if(IS_MULTIPLOT(viz_args))
    {
        fprintf(gnuplot,
                "set multiplot layout %i,%i rowsfirst\n",
                viz_args->rows, viz_args->cols);
    }

    for(r = 0; r < viz_args->rows; r++)
    {
        for(c = 0; c < viz_args->cols; c++)
        {
            fprintf(gnuplot, "plot sin(x) title \"Waiting for data\" with lines\n");
        }
    }

    if(IS_MULTIPLOT(viz_args))
        fprintf(gnuplot, "unset multiplot\n");
    fflush(gnuplot);

    // main loop
    while(1)
    {
        sem_acquire(&tfm_arg->signal);
        sem_acquire(&tfm_arg->lock);

        found = false;
        list_for_each_entry(curr, &tfm_arg->list, struct __tfm_viz_entry, list)
        {
            if(curr->count == NUMBER_TRACES(viz_args))
            {
                found = true;
                break;
            }
        }

        if(found) list_del(&curr->list);
        else
        {
            err("No finished block found after signal\n");
            ret = -EINVAL;
            goto __unlock;
        }

        critical("Displaying entry for base %li\n", curr->base);
        if(IS_MULTIPLOT(viz_args))
        {
            fprintf(gnuplot,
                    "set multiplot layout %i,%i rowsfirst\n",
                    viz_args->rows, viz_args->cols);
        }

        for(r = 0; r < viz_args->rows; r++)
        {
            for(c = 0; c < viz_args->cols; c++)
            {
                fprintf(gnuplot, "plot ");
                for(p = 0; p < viz_args->plots; p++)
                {
                    index = __plot_indices(viz_args, r, c, p);
                    if(curr->traces[index])
                    {
                        if(curr->traces[index]->samples)
                        {
                            fprintf(gnuplot,
                                    " '-' binary endian=little array=%i dx=%li "
                                    "format=\"%%f\" using 1 with lines",
                                    viz_args->samples, ts_num_samples(ts) /
                                                       viz_args->samples);

                            if(curr->traces[index]->title)
                                fprintf(gnuplot, " title \"%s\"",
                                        curr->traces[index]->title);

                            if(p != viz_args->plots - 1)
                                fprintf(gnuplot, ",");
                        }
                    }
                    else
                    {
                        err("Invalid entry for index %i\n", index);
                        ret = -EINVAL;
                        goto __unlock;
                    }
                }

                fprintf(gnuplot, "\n");
                for(p = 0; p < viz_args->plots; p++)
                {
                    index = __plot_indices(viz_args, r, c, p);
                    if(curr->traces[index])
                    {
                        for(s = 0; s < ts_num_samples(ts);
                            s += (int) (ts_num_samples(ts) / viz_args->samples))
                            fwrite(&curr->traces[index]->samples[s],
                                   sizeof(float), 1, gnuplot);
                    }
                }
            }
        }

        if(IS_MULTIPLOT(viz_args))
            fprintf(gnuplot, "unset multiplot\n");

        fflush(gnuplot);
        for(r = 0; r < NUMBER_TRACES(viz_args); r++)
            trace_free(curr->traces[r]);

        free(curr->traces);
        free(curr);

        sem_release(&tfm_arg->lock);
    }

__unlock:
    sem_release(&tfm_arg->lock);

__done:
    // todo close gnuplot
    tfm_arg->status = ret;
    return NULL;
#else
    err("Visualization not supported on non-Linux platforms\n");
    tfm_arg->status = -EINVAL;
    return NULL;
#endif
}

int __tfm_visualize_init(struct trace_set *ts)
{
    int ret;
    struct tfm_visualize_data *tfm_data;

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;

    tfm_data = calloc(1, sizeof(struct tfm_visualize_data));
    if(!tfm_data)
    {
        err("Failed to allocate set transformation data\n");
        return -ENOMEM;
    }

    tfm_data->currently_displayed = -1;
    ret = p_sem_create(&tfm_data->lock, 1);
    if(ret < 0)
    {
        err("Failed to initialize lock\n");
        ret = -errno;
        goto __free_tfm_data;
    }

    ret = p_sem_create(&tfm_data->signal, 0);
    if(ret < 0)
    {
        err("Failed to initialize signal\n");
        ret = -errno;
        goto __destroy_lock;
    }

    LIST_HEAD_INIT_INLINE(tfm_data->list);
    ts->tfm_state = tfm_data;
    tfm_data->viz_args = TFM_DATA(ts->tfm);

    ret = p_thread_create(&tfm_data->handle, __draw_gnuplot_thread, ts);
    if(ret < 0)
    {
        err("Failed to create draw thread\n");
        ret = -errno;
        ts->tfm_state = NULL;
        goto __destroy_signal;
    }

    return 0;
__destroy_signal:
    p_sem_destroy(&tfm_data->signal);

__destroy_lock:
    p_sem_destroy(&tfm_data->lock);

__free_tfm_data:
    free(tfm_data);
    return ret;
}

int __tfm_visualize_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_visualize_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_visualize_exit(struct trace_set *ts)
{
    struct tfm_visualize_data *tfm = ts->tfm_state;

//    sem_post(&tfm->signal);
    p_thread_join(tfm->handle);
}

int __tfm_visualize_fetch(struct trace *t)
{
    bool found;
    int ret, index;

    struct viz_args *tfm;
    struct tfm_visualize_data *tfm_data;
    struct __tfm_viz_entry *curr, *new = NULL;

    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    tfm = TFM_DATA(t->owner->tfm);
    tfm_data = t->owner->tfm_state;

    sem_acquire(&tfm_data->lock);
    curr = NULL, found = false;
    list_for_each_entry(curr, &tfm_data->list, struct __tfm_viz_entry, list)
    {
        if(TRACE_IDX(t) >= curr->base &&
           TRACE_IDX(t) < curr->base + NUMBER_TRACES(tfm))
        {
            found = true;
            break;
        }
    }

    debug("Index %zu got curr = %p (list_empty is %i, found is %i)\n",
          TRACE_IDX(t), curr, list_empty(&tfm_data->list), found);

    // not found
    if(!found)
    {
        if(tfm_data->currently_displayed != -1 &&
           TRACE_IDX(t) >= tfm_data->currently_displayed &&
           TRACE_IDX(t) < tfm_data->currently_displayed + NUMBER_TRACES(tfm))
        {
            debug("Fetch for %zu should pass through instead\n", TRACE_IDX(t));
            sem_release(&tfm_data->lock);
            return 1;
        }

        debug("Creating entry for base %zu because of trace %zu\n",
              TRACE_IDX(t) - (TRACE_IDX(t) % NUMBER_TRACES(tfm)), TRACE_IDX(t));

        new = calloc(1, sizeof(struct __tfm_viz_entry));
        if(!new)
        {
            err("Failed to allocate new viz entry\n");
            ret = -ENOMEM;
            goto __unlock;
        }

        new->base = TRACE_IDX(t) - (TRACE_IDX(t) % NUMBER_TRACES(tfm));
        new->count = 0;

        new->traces = calloc(NUMBER_TRACES(tfm), sizeof(struct trace *));
        if(!new->traces)
        {
            err("Failed to allocate new trace array\n");
            free(new); ret = -ENOMEM;
            goto __unlock;
        }

        LIST_HEAD_INIT_INLINE(new->list);
        list_for_each_entry(curr, &tfm_data->list, struct __tfm_viz_entry, list)
        {
            if(curr->base > new->base)
                break;
        }

        list_add_tail(&new->list, &curr->list);
        curr = new;
    }

    index = TRACE_IDX(t) % NUMBER_TRACES(tfm);
    if(!curr->traces[index])
    {
        sem_release(&tfm_data->lock);

        // could take a long time
        debug("Getting trace %zu\n", TRACE_IDX(t));
        ret = trace_get(t->owner->prev, &curr->traces[index], TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to get trace from previous set\n");
            return ret;
        }

        sem_acquire(&tfm_data->lock);

        curr->count++;
        critical("Got data for index %i in base %zu (%zu / %i)\n",
                 index, curr->base,
                 curr->count, NUMBER_TRACES(tfm));

        if(curr->count == NUMBER_TRACES(tfm))
        {
            debug("Signaling for base %zu\n", curr->base);
            if(tfm_data->status < 0)
            {
                err("Render thread has crashed\n");
                ret = tfm_data->status;
                goto __unlock;
            }

            tfm_data->currently_displayed = curr->base;
            sem_release(&tfm_data->signal);
        }
    }

    ret = copy_title(t, curr->traces[index]);
    if(ret >= 0)
        ret = copy_data(t, curr->traces[index]);

    if(ret >= 0)
        ret = copy_samples(t, curr->traces[index]);

    if(ret < 0)
    {
        err("Failed to copy something\n");
        passthrough_free(t);
        return ret;
    }

    ret = 0;
__unlock:
    sem_release(&tfm_data->lock);
    return ret;
}

int __tfm_visualize_get(struct trace *t)
{
    int ret;

    ret = __tfm_visualize_fetch(t);
    if(ret < 0)
    {
        err("Failed to get trace data from buffer\n");
        return ret;
    }

    if(ret == 1)
    {
        ret = passthrough(t);
        if(ret < 0)
        {
            err("Failed to passthrough title\n");
            return ret;
        }
    }

    return 0;
}

void __tfm_visualize_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_visualize(struct tfm **tfm, struct viz_args *args)
{
    struct tfm *res;
    res = (struct tfm *) calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_visualize);

    res->data = calloc(1, sizeof(struct viz_args));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    memcpy(TFM_DATA(res), args, sizeof(struct viz_args));
    *tfm = res;
    return 0;
}
