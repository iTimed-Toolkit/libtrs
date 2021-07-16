#include "trace.h"
#include "__trace_internal.h"
#include "__tfm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int trace_free_memory(struct trace *t)
{
    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    if(t->owner && t->owner->prev && t->owner->tfm)
        t->owner->tfm->free(t);
    else
    {
        if(t->title)
            free(t->title);

        if(t->data)
            free(t->data);

        if(t->samples)
            free(t->samples);
    }

    free(t);
    return 0;
}

int trace_get(struct trace_set *ts, struct trace **t, size_t index)
{
    int ret;
    struct trace *t_result;
    bool cache_missed = false;

    if(!ts || !t)
    {
        err("Invalid trace set or trace\n");
        return -EINVAL;
    }

    if(index >= ts_num_traces(ts))
    {
        err("Index %zu out of bounds for trace set\n", index);
        return -EINVAL;
    }

    debug("Getting trace %zu from trace set %zu\n", index, ts->set_id);
    if(ts->cache)
    {
        debug("Looking up trace %zu in cache\n", index);
        ret = tc_lookup(ts->cache, index, &t_result,
                        COHESIVE_CACHES ? true : false);
        if(ret < 0)
        {
            err("Failed to lookup trace in cache\n");
            return ret;
        }

        if(t_result) goto __done;
        else cache_missed = true;

        debug("Trace %zu not found in cache\n", index);
    }

    t_result = calloc(1, sizeof(struct trace));
    if(!t_result)
    {
        err("Failed to allocate memory for trace\n");
        return -ENOMEM;
    }

    t_result->owner = ts;
    t_result->index = index;
    t_result->title = NULL;
    t_result->data = NULL;
    t_result->samples = NULL;

    if(ts->prev && ts->tfm)
    {
        ret = ts->tfm->get(t_result);
        if(ret < 0)
        {
            err("Failed to get trace from transformation\n");
            goto __fail;
        }
    }
    else
    {
        ret = ts->backend->read(t_result);
        if(ret < 0)
        {
            err("Failed to read trace from file\n");
            goto __fail;
        }
    }

    if(cache_missed)
    {
        debug("Storing trace %zu in the cache\n", index);
        ret = tc_store(ts->cache, index, t_result,
                       COHESIVE_CACHES ? true : false);
        if(ret < 0)
        {
            err("Failed to store result trace in cache\n");
            goto __fail;
        }
    }

__done:
    *t = t_result;
    return 0;

__fail:
    trace_free_memory(t_result);
    *t = NULL;
    return ret;
}

int trace_copy(struct trace **res, struct trace *prev)
{
    int ret;
    struct trace *t_result;

    t_result = calloc(1, sizeof(struct trace));
    if(!t_result)
    {
        err("Failed to allocate memory for trace\n");
        return -ENOMEM;
    }

    if(prev->title)
    {
        t_result->title = calloc(prev->owner->title_size, sizeof(char));
        if(!t_result->title)
        {
            err("Failed to allocate memory for new trace title\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        memcpy(t_result->title,
               prev->title,
               prev->owner->title_size * sizeof(char));
    }
    else t_result->title = NULL;

    if(prev->data)
    {
        t_result->data = calloc(prev->owner->data_size, sizeof(uint8_t));
        if(!t_result->data)
        {
            err("Failed to allocate memory for new trace data\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        memcpy(t_result->data,
               prev->data,
               prev->owner->data_size * sizeof(uint8_t));
    }
    else t_result->data = NULL;

    if(prev->samples)
    {
        t_result->samples = calloc(prev->owner->num_samples, sizeof(float));
        if(!t_result->samples)
        {
            err("Failed to allocate memory for new trace samples\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        memcpy(t_result->samples,
               prev->samples,
               prev->owner->num_samples * sizeof(float));

    }
    else t_result->samples = NULL;

    *res = t_result;
    return 0;

__fail_free_result:
    trace_free_memory(t_result);
    return ret;
}

int trace_free(struct trace *t)
{
    if(t->owner->cache)
    {
        debug("Dereferencing trace in cache\n");
        // this will cause a free if it needs to
        return tc_deref(t->owner->cache, TRACE_IDX(t), t);
    }
    else
    {
        debug("Freeing trace memory\n");
        return trace_free_memory(t);
    }
}