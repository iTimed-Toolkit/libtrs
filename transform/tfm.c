#include "__tfm_internal.h"

#include "libtrace.h"
#include "__trace_internal.h"

#include <string.h>
#include <errno.h>

int passthrough_title(struct trace *trace, char **title)
{
    int ret;
    char *prev_title, *res;
    struct trace *prev_trace;

    ret = trace_get(trace->owner->prev, &prev_trace, TRACE_IDX(trace), false);
    if(ret < 0)
    {
        err("Failed to get trace from previous trace set\n");
        return ret;
    }

    ret = trace_title(prev_trace, &prev_title);
    if(ret < 0)
    {
        err("Failed to get title from previous trace\n");
        goto __out;
    }

    if(prev_title)
    {
        res = calloc(trace->owner->title_size, sizeof(char));
        if(!res)
        {
            err("Failed to allocate memory for trace title\n");
            return -ENOMEM;
        }

        memcpy(res, prev_title, trace->owner->title_size);
        *title = res;
    }
    else *title = NULL;
    ret = 0;

__out:
    trace_free(prev_trace);
    return ret;
}

int passthrough_data(struct trace *trace, uint8_t **data)
{
    int ret;
    uint8_t *prev_data, *res;
    struct trace *prev_trace;

    ret = trace_get(trace->owner->prev, &prev_trace, TRACE_IDX(trace), false);
    if(ret < 0)
    {
        err("Failed to get trace from previous trace set\n");
        return ret;
    }

    ret = trace_data_all(prev_trace, &prev_data);
    if(ret < 0)
    {
        err("Failed to get data from previous trace\n");
        goto __out;
    }

    if(prev_data)
    {
        res = calloc(trace->owner->data_size, sizeof(uint8_t));
        if(!res)
        {
            err("Failed to allocate memory for trace data\n");
            return -ENOMEM;
        }

        memcpy(res, prev_data, trace->owner->data_size);
        *data = res;
    }
    else *data = NULL;
    ret = 0;

__out:
    trace_free(prev_trace);
    return ret;
}

int passthrough_samples(struct trace *trace, float **samples)
{
    int ret;
    float *prev_samples, *res;
    struct trace *prev_trace;

    ret = trace_get(trace->owner->prev, &prev_trace, TRACE_IDX(trace), false);
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
        res = calloc(trace->owner->num_samples, sizeof(float));
        if(!res)
        {
            err("Failed to allocate memory for trace samples\n");
            return -ENOMEM;
        }

        memcpy(res, prev_samples, trace->owner->num_samples * sizeof(float));
        *samples = res;
    }
    else *samples = NULL;
    ret = 0;

__out:
    trace_free(prev_trace);
    return ret;
}

void passthrough_free_title(struct trace *t)
{
    if(t->buffered_title)
        free(t->buffered_title);
}

void passthrough_free_data(struct trace *t)
{
    if(t->buffered_data)
        free(t->buffered_data);
}

void passthrough_free_samples(struct trace *t)
{
    if(t->buffered_samples)
        free(t->buffered_samples);
}