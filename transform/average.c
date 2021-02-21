#include "transform.h"
#include "libtrs.h"
#include "../lib/__libtrs_internal.h"

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

    ts->title_size = strlen("Average");
    ts->data_size = 0;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
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
    int i, j, ret;
    char *title;
    struct trace *curr;
    struct tfm_average *tfm = TFM_DATA(t->owner->tfm);
    float *result = calloc(t->owner->num_samples, sizeof(float)),
            *curr_samples;

    if(!result)
        return -ENOMEM;

    if(tfm->per_sample)
    {
        for(i = 0; i < t->owner->prev->num_traces; i++)
        {
            ret = trace_get(t->owner->prev, &curr, i, false);
            if(ret < 0)
                goto __free_result;

            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
                goto __free_trace;

            trace_title(curr, &title);
            if(title) printf("%s\n", title);

            if(curr_samples)
            {
                // todo make more cache-efficient
                for(j = 0; j < t->owner->prev->num_samples; j++)
                    result[j] += curr_samples[j];
            }

            trace_free(curr);
        }

        for(i = 0; i < t->owner->prev->num_samples; i++)
            result[i] /= t->owner->prev->num_traces;
    }
    else
    {
        for(i = 0; i < t->owner->prev->num_traces; i++)
        {
            ret = trace_get(t->owner->prev, &curr, i, false);
            if(ret < 0)
                goto __free_result;

            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
                goto __free_trace;

            trace_title(curr, &title);
            if(title) printf("%s\n", title);

            if(curr_samples)
            {
                // todo index different to avoid empty spots?
                for(j = 0; j < t->owner->prev->num_samples; j++)
                    result[i] += curr_samples[j];
                result[i] /= t->owner->prev->num_samples;
            }

            trace_free(curr);
        }
    }

    *samples = result;
    return 0;

__free_trace:
    trace_free(curr);

__free_result:
    free(result);
    *samples = NULL;
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
    res = calloc(1, sizeof(struct tfm_average));
    if(!res)
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_average);

    res->tfm_data = calloc(1, sizeof(struct tfm_average));
    if(!res->tfm_data)
    {
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->per_sample = per_sample;
    *tfm = res;
    return 0;
}