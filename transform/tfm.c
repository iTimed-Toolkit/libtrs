#include "__tfm_internal.h"

#include "trace.h"
#include "__trace_internal.h"

#include "statistics.h"
#include "list.h"

#include <string.h>
#include <errno.h>

int copy_title(struct trace *to, struct trace *from)
{
    char *res;
    if(to->owner->title_size != from->owner->title_size)
    {
        err("Incompatible title sizes\n");
        return -EINVAL;
    }

    if(from->title)
    {
        res = calloc(from->owner->title_size, sizeof(char));
        if(!res)
        {
            err("Failed to allocate memory for trace title\n");
            return -ENOMEM;
        }

        memcpy(res, from->title, to->owner->title_size);
        to->title = res;
    }
    else to->title = NULL;

    return 0;
}

int copy_data(struct trace *to, struct trace *from)
{
    uint8_t *res;
    if(to->owner->data_size != from->owner->data_size)
    {
        err("Incompatible data sizes\n");
        return -EINVAL;
    }

    if(from->data)
    {
        res = calloc(from->owner->data_size, sizeof(uint8_t));
        if(!res)
        {
            err("Failed to allocate memory for trace data\n");
            return -ENOMEM;
        }

        memcpy(res, from->data, to->owner->data_size);
        to->data = res;
    }
    else to->data = NULL;

    return 0;
}

int copy_samples(struct trace *to, struct trace *from)
{
    float *res;
    if(to->owner->num_samples != from->owner->num_samples)
    {
        err("Incompatible data sizes\n");
        return -EINVAL;
    }

    if(from->samples)
    {
        res = calloc(from->owner->num_samples, sizeof(float));
        if(!res)
        {
            err("Failed to allocate memory for trace samples\n");
            return -ENOMEM;
        }

        memcpy(res, from->samples, to->owner->num_samples * sizeof(float));
        to->samples = res;
    }
    else to->samples = NULL;

    return 0;
}

int passthrough(struct trace *trace)
{
    int ret;
    struct trace *prev_trace;

    if(trace->title || trace->data || trace->samples)
    {
        err("Called with existing member arrays\n");
        return -EINVAL;
    }

    ret = trace_get(trace->owner->prev, &prev_trace, TRACE_IDX(trace));
    if(ret < 0)
    {
        err("Failed to get trace from previous trace set\n");
        return ret;
    }

    ret = copy_title(trace, prev_trace);
    if(ret >= 0)
        ret = copy_data(trace, prev_trace);

    if(ret >= 0)
        ret = copy_samples(trace, prev_trace);

    if(ret < 0)
    {
        err("Failed to copy something\n");
        goto __fail;
    }

    trace_free(prev_trace);
    return 0;

__fail:
    if(trace->title)
        free(trace->title);

    if(trace->data)
        free(trace->data);

    if(trace->samples)
        free(trace->samples);

    trace_free(prev_trace);
    return ret;
}

void passthrough_free(struct trace *t)
{
    if(t->title)
        free(t->title);

    if(t->data)
        free(t->data);

    if(t->samples)
        free(t->samples);
}

// this is used by various transformations
stat_t __summary_to_cability(summary_t s)
{
    switch(s)
    {
        case SUMMARY_AVG:
            return STAT_AVG;
        case SUMMARY_DEV:
            return STAT_DEV;
        case SUMMARY_MIN:
            return STAT_MIN;
        case SUMMARY_MAX:
            return STAT_MAX;
        case SUMMARY_MINABS:
            return STAT_MINABS;
        case SUMMARY_MAXABS:
            return STAT_MAXABS;
        default:
        err("Invalid summary\n");
            return 0;
    }
}