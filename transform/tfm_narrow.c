#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <string.h>

#define TFM_DATA(tfm)   ((struct tfm_narrow *) (tfm)->tfm_data)

struct tfm_narrow
{
    int first_trace, num_traces;
    int first_sample, num_samples;
};

int __tfm_narrow_init(struct trace_set *ts)
{
    struct tfm_narrow *tfm = TFM_DATA(ts->tfm);

    ts->num_samples = tfm->num_samples;
    ts->num_traces = tfm->num_traces;

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
    return 0;
}

size_t __tfm_narrow_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __tfm_narrow_title(struct trace *t, char **title)
{
    int ret;
    struct tfm_narrow *tfm = TFM_DATA(t->owner->tfm);

    t->start_offset += tfm->first_trace;
    ret = passthrough_title(t, title);
    t->start_offset -= tfm->first_trace;

    return ret;
}

int __tfm_narrow_data(struct trace *t, uint8_t **data)
{
    int ret;
    struct tfm_narrow *tfm = TFM_DATA(t->owner->tfm);

    t->start_offset += tfm->first_trace;
    ret = passthrough_data(t, data);
    t->start_offset -= tfm->first_trace;

    return ret;
}

int __tfm_narrow_samples(struct trace *t, float **samples)
{
    int ret;
    float *prev_samples, *res;
    struct trace *prev_trace;

    struct tfm_narrow *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t) + tfm->first_trace, false);
    if(ret < 0)
    {
        err("Failed to get trace from previous trace set\n");
        return ret;
    }

    ret = trace_samples(prev_trace, &prev_samples);
    if(ret < 0)
    {
        err("Failed to get samples from previous trace\n");
        goto __out;
    }

    if(prev_samples)
    {
        res = calloc(t->owner->num_samples, sizeof(float));
        if(!res)
        {
            err("Failed to allocate memory for trace samples\n");
            return -ENOMEM;
        }

        memcpy(res, &prev_samples[tfm->first_sample],
               t->owner->num_samples * sizeof(float));
        *samples = res;
    }
    else *samples = NULL;
    ret = 0;

__out:
    trace_free(prev_trace);
    return ret;
}

void __tfm_narrow_exit(struct trace_set *ts)
{}

void __tfm_narrow_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_narrow_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_narrow_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_narrow(struct tfm **tfm,
               int first_trace, int num_traces,
               int first_sample, int num_samples)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_narrow);

    res->tfm_data = calloc(1, sizeof(struct tfm_narrow));
    if(!res->tfm_data)
    {
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->first_trace = first_trace;
    TFM_DATA(res)->num_traces = num_traces;
    TFM_DATA(res)->first_sample = first_sample;
    TFM_DATA(res)->num_samples = num_samples;

    *tfm = res;
    return 0;
}