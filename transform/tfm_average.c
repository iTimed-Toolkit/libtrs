#include "transform.h"
#include "trace.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TFM_DATA(tfm)   ((struct tfm_average *) (tfm)->tfm_data)

struct tfm_average
{
    bool per_sample;
};

int __tfm_average_init(struct trace_set *ts)
{
    struct tfm_average *tfm = TFM_DATA(ts->tfm);
    if(tfm->per_sample)
    {
        ts->num_samples = ts->prev->num_samples;
        ts->num_traces = 1;
    }
    else
    {
        ts->num_samples = ts->prev->num_traces;
        ts->num_traces = 1;
    }

    ts->input_offs = ts->input_len =
    ts->output_offs = ts->output_len =
    ts->key_offs = ts->key_len = 0;

    ts->title_size = strlen("Average") + 1;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1;
    return 0;
}

int __tfm_average_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_average_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __tfm_average_title(struct trace *t, char **title)
{
    *title = "Average";
    return 0;
}

int __tfm_average_data(struct trace *t, uint8_t **data)
{
    *data = NULL;
    return 0;
}

int __tfm_average_samples(struct trace *t, float **samples)
{
    int i, ret;
    float *result = NULL, *curr_samples;

    struct trace *curr;
    struct accumulator *acc;
    struct tfm_average *tfm = TFM_DATA(t->owner->tfm);

    if(tfm->per_sample)
    {
        ret = stat_create_single_array(&acc, ts_num_samples(t->owner->prev));
        if(ret < 0)
        {
            err("Failed to create accumulator\n");
            return ret;
        }

        for(i = 0; i < ts_num_traces(t->owner->prev); i++)
        {
            ret = trace_get(t->owner->prev, &curr, i, false);
            if(ret < 0)
            {
                err("Failed to get trace from previous trace set\n");
                goto __free_accumulator;
            }

            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
            {
                err("Failed to get samples to average from trace\n");
                goto __free_accumulator;
            }

            if(curr_samples)
            {
                ret = stat_accumulate_single_array(acc, curr_samples,
                                                   ts_num_samples(t->owner->prev));
                if(ret < 0)
                {
                    err("Failed to accumulate trace %li\n", TRACE_IDX(curr));
                    goto __free_accumulator;
                }
            }

            trace_free(curr);
            curr = NULL;
        }

        ret = stat_get_mean_all(acc, samples);
        if(ret < 0)
        {
            err("Failed to get mean from accumulator\n");
            goto __free_accumulator;
        }
    }
    else
    {
        ret = stat_create_single(&acc);
        if(ret < 0)
        {
            err("Failed to create accumulator\n");
            return ret;
        }

        result = calloc(ts_num_traces(t->owner), sizeof(float));
        if(!result)
        {
            err("Failed to allocate buffer for accumulating average\n");
            goto __free_result;
        }

        for(i = 0; i < ts_num_traces(t->owner->prev); i++)
        {
            ret = trace_get(t->owner->prev, &curr, i, false);
            if(ret < 0)
            {
                err("Failed to get trace from previous trace set\n");
                goto __free_result;
            }

            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
            {
                err("Failed to get samples to average from trace\n");
                goto __free_trace;
            }

            if(curr_samples)
            {
                ret = stat_accumulate_single_many(acc, curr_samples,
                                                  ts_num_samples(t->owner->prev));
                if(ret < 0)
                {
                    err("Failed to accumulate trace %li\n", TRACE_IDX(curr));
                    goto __free_accumulator;
                }

                ret = stat_get_mean(acc, 0, &result[i]);
                if(ret < 0)
                {
                    err("Failed to get mean for trace %li\n", TRACE_IDX(curr));
                    goto __free_accumulator;
                }

                stat_reset_accumulator(acc);
            }

            trace_free(curr);
        }

        *samples = result;
        result = NULL;
    }

__free_accumulator:
    stat_free_accumulator(acc);

__free_trace:
    if(curr)
        trace_free(curr);

__free_result:
    if(result)
        free(result);

    return ret;
}

void __tfm_average_exit(struct trace_set *ts)
{}

void __tfm_average_free_title(struct trace *t)
{}

void __tfm_average_free_data(struct trace *t)
{}

void __tfm_average_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_average(struct tfm **tfm, bool per_sample)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_average);

    res->tfm_data = calloc(1, sizeof(struct tfm_average));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->per_sample = per_sample;
    *tfm = res;
    return 0;
}