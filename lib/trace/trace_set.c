#include "trace.h"
#include "__trace_internal.h"
#include "__tfm_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

size_t gbl_set_index = 0;

int ts_open(struct trace_set **ts, const char *path)
{
    int ret;
    struct trace_set *ts_result;

    if(!ts || !path)
    {
        err("Invalid trace set or path\n");
        return -EINVAL;
    }

    ts_result = calloc(1, sizeof(struct trace_set));
    if(!ts_result)
    {
        err("Trace set allocation failed\n");
        return -ENOMEM;
    }

    ts_result->set_id = __atomic_fetch_add(&gbl_set_index, 1, __ATOMIC_RELAXED);
    debug("Creating new trace set with ID %li\n", ts_result->set_id);

    ret = create_backend(ts_result, path);
    if(ret < 0)
    {
        err("Failed to create backend for trace set\n");
        goto __free_ts_result;
    }

    ret = ts_result->backend->open(ts_result);
    if(ret < 0)
    {
        err("Failed to open backend for trace set\n");
        goto __free_backend;
    }

    ts_result->cache = NULL;
    ts_result->prev = NULL;
    ts_result->tfm = NULL;
    ts_result->tfm_state = NULL;
    ts_result->tfm_next = NULL;
    ts_result->tfm_next_arg = NULL;

    *ts = ts_result;
    return 0;

__free_backend:
    ts_result->backend->close(ts_result);

__free_ts_result:
    free(ts_result);

    *ts = NULL;
    return ret;
}

int ts_close(struct trace_set *ts)
{
    int ret;
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    debug("Closing trace set %li\n", ts->set_id);

    // transform specific teardown
    if(ts->prev && ts->tfm)
        ts->tfm->exit(ts);

    if(ts->backend)
    {
        ret = ts->backend->close(ts);
        if(ret < 0)
        {
            err("Failed to close backend\n");
            return ret;
        }
    }

    if(ts->cache)
        tc_free(ts->cache);

    free(ts);
    return 0;
}

int ts_transform(struct trace_set **new_ts, struct trace_set *prev, struct tfm *transform)
{
    int ret;
    struct trace_set *ts_result;

    if(!new_ts || !prev || !transform)
    {
        err("Invalid trace sets or transform\n");
        return -EINVAL;
    }

    ts_result = calloc(1, sizeof(struct trace_set));
    if(!ts_result)
    {
        err("Trace set allocation failed\n");
        return -ENOMEM;
    }

    ts_result->set_id = __atomic_fetch_add(&gbl_set_index, 1, __ATOMIC_RELAXED);
    ts_result->backend = NULL;
    ts_result->cache = NULL;

    // link previous set
    ts_result->prev = prev;
    ts_result->tfm = transform;
    ts_result->tfm_state = NULL;
    ts_result->tfm_next = NULL;
    ts_result->tfm_next_arg = NULL;

    debug("Creating transformed trace set with ID %li\n", ts_result->set_id);

    // transform-specific initialization
    ret = transform->init(ts_result);
    if(ret < 0)
    {
        err("Failed to initialize transform\n");
        free(ts_result);
        return ret;
    }

    *new_ts = ts_result;
    return 0;
}

size_t ts_num_traces(struct trace_set *ts)
{
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    return ts->num_traces;
}

size_t ts_num_samples(struct trace_set *ts)
{
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    return ts->num_samples;
}

size_t ts_trace_size(struct trace_set *ts)
{
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    if(ts->prev && ts->tfm)
        return ts->tfm->trace_size(ts);
    else
        return ts->num_samples * sizeof(float) +
               ts->data_size + ts->title_size +
               sizeof(struct trace);
}
