#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <string.h>

#define TFM_DATA(tfm)   ((struct viz_args *) (tfm)->tfm_data)
#define GNUPLOT_CMD     "gnuplot"

#define IS_MULTIPLOT(tfm)   ((tfm)->rows != 1 || (tfm)->cols != 1)
#define NUMBER_TRACES(tfm)  ((tfm)->rows * (tfm)->cols * (tfm)->plots)

struct tfm_visualize_data
{
    FILE *gnuplot;
    size_t current_base, current_count;

    sem_t lock;
    char **current_titles;
    float **current_samples;
    bool *current_valid;
};

int __plot_indices(struct viz_args *tfm, int r, int c, int p)
{
    if(tfm->fill_order[0] == PLOTS)
    {
        if(tfm->fill_order[1] == ROWS)
            return r * (tfm->cols * tfm->plots) + c * tfm->plots + p;
        else if(tfm->fill_order[1] == COLS)
            return c * (tfm->rows * tfm->plots) + r * tfm->plots + p;
    }
    else if(tfm->fill_order[0] == ROWS)
    {
        if(tfm->fill_order[1] == PLOTS)
            return r * (tfm->cols * tfm->plots) + c + (tfm->cols) * p;
        else if(tfm->fill_order[1] == COLS)
            return r + c + (tfm->rows * tfm->cols) * p;
    }
    else if(tfm->fill_order[0] == COLS)
    {
        if(tfm->fill_order[1] == ROWS)
            return c * tfm->rows + r + (tfm->rows * tfm->cols) * p;
        else if(tfm->fill_order[1] == PLOTS)
            return c * (tfm->rows * tfm->plots) + r + (tfm->rows) * p;
    }

    return -1;
}

int __tfm_visualize_init(struct trace_set *ts)
{
    int ret, r, c;
    struct viz_args *tfm = TFM_DATA(ts->tfm);
    struct tfm_visualize_data *tfm_data;

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->input_offs = ts->prev->input_offs;
    ts->input_len = ts->prev->input_len;
    ts->output_offs = ts->prev->output_offs;
    ts->output_len = ts->prev->output_len;
    ts->key_offs = ts->prev->key_offs;
    ts->key_len = ts->prev->key_len;

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

    tfm_data->current_titles = calloc(NUMBER_TRACES(tfm), sizeof(char *));
    if(!tfm_data->current_titles)
    {
        err("Failed to allocate title buffer\n");
        ret = -ENOMEM;
        goto __free_tfm_data;
    }

    tfm_data->current_samples = calloc(NUMBER_TRACES(tfm), sizeof(float *));
    if(!tfm_data->current_samples)
    {
        err("Failed to allocate sample buffer\n");
        ret = -ENOMEM;
        goto __free_tfm_data;
    }

    tfm_data->current_valid = calloc(NUMBER_TRACES(tfm), sizeof(bool));
    if(!tfm_data->current_valid)
    {
        err("Failed to allocate valid buffer\n");
        ret = -ENOMEM;
        goto __free_tfm_data;
    }

    ret = sem_init(&tfm_data->lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize lock\n");
        ret = -errno;
        goto __free_tfm_data;
    }

    tfm_data->gnuplot = popen(GNUPLOT_CMD, "w");
    if(!tfm_data->gnuplot)
    {
        err("Failed to open gnuplot -- installed?\n");
        ret = -errno;
        goto __free_tfm_data;
    }

    tfm_data->current_base = 0;
    tfm_data->current_count = 0;

    fprintf(tfm_data->gnuplot, "set term x11\n");
    fprintf(tfm_data->gnuplot, "set grid\n");
    if(tfm->samples == 0)
    {
        fprintf(tfm_data->gnuplot, "set samples %li\n", ts_num_samples(ts));
        tfm->samples = ts_num_samples(ts);
    }
    else
        fprintf(tfm_data->gnuplot, "set samples %i\n", tfm->samples);
//    fprintf(tfm_data->gnuplot, "unset key\n");

    if(IS_MULTIPLOT(tfm))
    {
        fprintf(tfm_data->gnuplot,
                "set multiplot layout %i,%i rowsfirst\n",
                tfm->rows, tfm->cols);
    }

    for(r = 0; r < tfm->rows; r++)
    {
        for(c = 0; c < tfm->cols; c++)
        {
            fprintf(tfm_data->gnuplot, "plot sin(x) title \"Waiting for data\" with lines\n");
        }
    }

    if(IS_MULTIPLOT(tfm))
        fprintf(tfm_data->gnuplot, "unset multiplot\n");
    fflush(tfm_data->gnuplot);

    ts->tfm_data = tfm_data;
    return 0;

__free_tfm_data:
    if(tfm_data->current_titles &&
       tfm_data->current_samples &&
       tfm_data->current_valid)
        sem_destroy(&tfm_data->lock);

    if(tfm_data->current_titles)
        free(tfm_data->current_titles);

    if(tfm_data->current_samples)
        free(tfm_data->current_samples);

    if(tfm_data->current_valid)
        free(tfm_data->current_valid);

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

int __redraw_gnuplot(struct trace_set *ts, struct viz_args *tfm,
                     struct tfm_visualize_data *tfm_data)
{
    int r, c, p, s;
    int index;

    if(IS_MULTIPLOT(tfm))
    {
        fprintf(tfm_data->gnuplot,
                "set multiplot layout %i,%i rowsfirst\n",
                tfm->rows, tfm->cols);
    }

    for(r = 0; r < tfm->rows; r++)
    {
        for(c = 0; c < tfm->cols; c++)
        {
            fprintf(tfm_data->gnuplot, "plot ");
            for(p = 0; p < tfm->plots; p++)
            {
                index = __plot_indices(tfm, r, c, p);
                if(tfm_data->current_valid[index])
                {
                    if(tfm_data->current_samples[index])
                    {
                        fprintf(tfm_data->gnuplot,
                                " '-' binary endian=little array=%i dx=%li "
                                "format=\"%%f\" using 1 with lines",
                                tfm->samples, ts_num_samples(ts) / tfm->samples);

                        if(tfm_data->current_titles[index])
                            fprintf(tfm_data->gnuplot, " title \"%s\"",
                                    tfm_data->current_titles[index]);

                        // todo this might be wrong
                        if(p != tfm->plots - 1)
                            fprintf(tfm_data->gnuplot, ",");
                    }
                }
                else
                {
                    err("...");
                }
            }

            fprintf(tfm_data->gnuplot, "\n");
            for(p = 0; p < tfm->plots; p++)
            {
                index = __plot_indices(tfm, r, c, p);
                if(tfm_data->current_samples[index])
                {
                    for(s = 0; s < ts_num_samples(ts); s += (int) (ts_num_samples(ts) / tfm->samples))
                        fwrite(&tfm_data->current_samples[index][s],
                               sizeof(float), 1, tfm_data->gnuplot);
                }
            }
        }
    }

    if(IS_MULTIPLOT(tfm))
        fprintf(tfm_data->gnuplot, "unset multiplot\n");

    fflush(tfm_data->gnuplot);
    tfm_data->current_base += NUMBER_TRACES(tfm);
    tfm_data->current_count = 0;

    for(r = 0; r < NUMBER_TRACES(tfm); r++)
    {
        if(tfm_data->current_titles[r])
        {
            free(tfm_data->current_titles[r]);
            tfm_data->current_titles[r] = NULL;
        }

        if(tfm_data->current_samples[r])
        {
            free(tfm_data->current_samples[r]);
            tfm_data->current_samples[r] = NULL;
        }

        tfm_data->current_valid[r] = false;
    }

    return 0;
}

int __tfm_visualize_fetch(struct trace *t, char *title, float *samples)
{
    int ret, index;
    struct trace *prev;
    struct viz_args *tfm;
    struct tfm_visualize_data *tfm_data;

    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    tfm = TFM_DATA(t->owner->tfm);
    tfm_data = t->owner->tfm_data;

    ret = sem_wait(&tfm_data->lock);
    if(ret < 0)
    {
        err("Failed to lock data buffers\n");
        return -errno;
    }

    index = TRACE_IDX(t) % NUMBER_TRACES(tfm);
    if(tfm_data->current_valid[index])
    {
        if(title)
            memcpy(title, tfm_data->current_titles[index],
                   t->owner->title_size * sizeof(char));

        if(samples)
            memcpy(samples, tfm_data->current_samples[index],
                   t->owner->num_samples * sizeof(float));
    }
    else
    {
        ret = trace_get(t->owner->prev, &prev, TRACE_IDX(t), false);
        if(ret < 0)
        {
            err("Failed to get trace from previous set\n");
            return ret;
        }

        ret = trace_title(prev, &tfm_data->current_titles[index]);
        if(ret < 0)
        {
            err("Failed to get title from previous trace\n");
            return ret;
        }

        ret = trace_samples(prev, &tfm_data->current_samples[index]);
        if(ret < 0)
        {
            err("Failed to get samples from previous trace\n");
            return ret;
        }

        tfm_data->current_valid[index] = true;
        tfm_data->current_count++;

        if(title)
            memcpy(title, tfm_data->current_titles[index], t->owner->title_size * sizeof(char));

        if(samples)
            memcpy(samples, tfm_data->current_samples[index], t->owner->num_samples * sizeof(float));
    }

    ret = sem_post(&tfm_data->lock);
    if(ret < 0)
    {
        err("Failed to post to data buffers\n");
        return -errno;
    }

    return 0;

}

int __tfm_visualize_title(struct trace *t, char **title)
{
    int ret;
    char *result;
    struct viz_args *tfm;
    struct tfm_visualize_data *tfm_data;

    if(!t || !title)
    {
        err("Invalid trace or destination pointer\n");
        return -EINVAL;
    }

    tfm = TFM_DATA(t->owner->tfm);
    tfm_data = t->owner->tfm_data;

    if(TRACE_IDX(t) >= tfm_data->current_base + NUMBER_TRACES(tfm) &&
       tfm_data->current_count == NUMBER_TRACES(tfm))
        __redraw_gnuplot(t->owner, tfm, tfm_data);

    if(TRACE_IDX(t) >= tfm_data->current_base &&
       TRACE_IDX(t) < tfm_data->current_base + NUMBER_TRACES(tfm))
    {
        result = calloc(t->owner->title_size, sizeof(char));
        if(!result)
        {
            err("Failed to allocate memory for title\n");
            return -ENOMEM;
        }

        ret = __tfm_visualize_fetch(t, result, NULL);
        if(ret < 0)
        {
            err("Failed to get trace data from buffer\n");
            free(result);
            return ret;
        }

        *title = result;
        return 0;
    }
    else return passthrough_title(t, title);
}

int __tfm_visualize_data(struct trace *t, uint8_t **data)
{
    return passthrough_data(t, data);
}

int __tfm_visualize_samples(struct trace *t, float **samples)
{
    int ret;
    float *result;
    struct viz_args *tfm;
    struct tfm_visualize_data *tfm_data;

    if(!t || !samples)
    {
        err("Invalid trace or destination pointer\n");
        return -EINVAL;
    }

    tfm = TFM_DATA(t->owner->tfm);
    tfm_data = t->owner->tfm_data;

    if(TRACE_IDX(t) >= tfm_data->current_base + NUMBER_TRACES(tfm) &&
       tfm_data->current_count == NUMBER_TRACES(tfm))
        __redraw_gnuplot(t->owner, tfm, tfm_data);

    if(TRACE_IDX(t) >= tfm_data->current_base &&
       TRACE_IDX(t) < tfm_data->current_base + NUMBER_TRACES(tfm))
    {
        result = calloc(t->owner->num_samples, sizeof(float));
        if(!result)
        {
            err("Failed to allocate memory for samples\n");
            return -ENOMEM;
        }

        ret = __tfm_visualize_fetch(t, NULL, result);
        if(ret < 0)
        {
            err("Failed to get trace data from buffer\n");
            free(result);
            return ret;
        }

        *samples = result;
        return 0;
    }
    else return passthrough_samples(t, samples);
}

void __tfm_visualize_exit(struct trace_set *ts)
{}

void __tfm_visualize_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_visualize_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_visualize_free_samples(struct trace *t)
{
    passthrough_free_samples(t);
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

    res->tfm_data = calloc(1, sizeof(struct viz_args));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    memcpy(TFM_DATA(res), args, sizeof(struct viz_args));
    *tfm = res;
    return 0;
}
