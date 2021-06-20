#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <string.h>

#define TFM_DATA(tfm)   ((struct tfm_narrow *) (tfm)->data)

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

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_narrow_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_narrow_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}


void __tfm_narrow_exit(struct trace_set *ts)
{}

int __tfm_narrow_get(struct trace *t)
{
    int ret;
    float *res;

    struct trace *prev_trace;
    struct tfm_narrow *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t) + tfm->first_trace);
    if(ret < 0)
    {
        err("Failed to get previous trace\n");
        return ret;
    }

    ret = copy_title(t, prev_trace);
    if(ret >= 0)
        ret = copy_data(t, prev_trace);

    if(ret >= 0)
    {
        if(prev_trace->samples)
        {
            res = calloc(t->owner->num_samples, sizeof(float));
            if(!res)
            {
                err("Failed to allocate memory for trace samples\n");
                ret = -ENOMEM;
                goto __out;
            }

            memcpy(res, &prev_trace->samples[tfm->first_sample],
                   t->owner->num_samples * sizeof(float));
            t->samples = res;
        }

        ret = 0;
    }
    else err("Failed to copy something\n");

__out:
    trace_free(prev_trace);
    return ret;
}

void __tfm_narrow_free(struct trace *t)
{
    passthrough_free(t);
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

    res->data = calloc(1, sizeof(struct tfm_narrow));
    if(!res->data)
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