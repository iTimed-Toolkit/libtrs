#include "libtrs.h"
#include "__libtrs_internal.h"
#include "__tfm_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

size_t gbl_set_index = 0;

int ts_open(struct trace_set **ts, const char *path)
{
    int retval;
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

    ts_result->set_id = __sync_fetch_and_add(&gbl_set_index, 1);
    debug("Creating new trace set with ID %li\n", ts_result->set_id);

    ts_result->ts_file = fopen(path, "rb");
    if(!ts_result->ts_file)
    {
        err("Unable to open trace set at %s: %s\n", path, strerror(errno));
        retval = -errno;
        goto __free_ts_result;
    }

    retval = init_headers(ts_result);
    if(retval < 0)
    {
        err("Failed to initialize trace set headers\n");
        goto __close_ts_file;
    }

#if SUPPORT_PTHREAD
    retval = sem_init(&ts_result->file_lock, 0, 1);
    if(retval < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        retval = -errno;
        goto __free_headers;
    }
#endif

    ts_result->prev = NULL;
    ts_result->tfm = NULL;
    ts_result->cache = NULL;

    *ts = ts_result;
    return 0;

__free_headers:
    free_headers(ts_result);

__close_ts_file:
    fclose(ts_result->ts_file);

__free_ts_result:
    free(ts_result);

    *ts = NULL;
    return retval;
}

int ts_close(struct trace_set *ts)
{
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    debug("Closing trace set %li\n", ts->set_id);

    // wait for any consumers
    ts_lock(ts, ;)

#if SUPPORT_PTHREAD
    sem_destroy(&ts->file_lock);
#endif

    if(ts->headers)
        free_headers(ts);

    if(ts->ts_file)
        fclose(ts->ts_file);

    if(ts->prev && ts->tfm)
        ts->tfm->exit(ts);

    if(ts->cache)
        tc_free(ts);

    free(ts);
    return 0;
}

int ts_create(struct trace_set **ts, struct trace_set *from, const char *path)
{
    err("Unsupported\n");
    return -1;
}

int ts_append(struct trace_set *ts, struct trace *t)
{
    err("Unsupported\n");
    return -1;
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

    // no need to seek within a file or parse headers
    ts_result->ts_file = NULL;
    ts_result->trace_length = 1;
    ts_result->trace_start = 0;

    ts_result->num_headers = 0;
    ts_result->headers = NULL;

    // link previous set
    ts_result->set_id = __sync_fetch_and_add(&gbl_set_index, 1);
    ts_result->cache = NULL;
    ts_result->prev = prev;
    ts_result->tfm = transform;

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
