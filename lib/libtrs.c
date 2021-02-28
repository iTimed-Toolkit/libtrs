#include "libtrs.h"
#include "__libtrs_internal.h"
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
    debug("Reading new trace set with ID %li\n", ts_result->set_id);

    ts_result->ts_file = fopen(path, "rb");
    if(!ts_result->ts_file)
    {
        err("Unable to open trace set at %s: %s\n", path, strerror(errno));
        ret = -errno;
        goto __free_ts_result;
    }

    ret = read_headers(ts_result);
    if(ret < 0)
    {
        err("Failed to initialize trace set headers\n");
        goto __close_ts_file;
    }

    ret = sem_init(&ts_result->file_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_headers;
    }

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
    return ret;
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
    sem_wait(&ts->file_lock);
    sem_destroy(&ts->file_lock);

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
    int ret;
    struct trace_set *ts_result;

    if(!ts || !from || !path)
    {
        err("Invalid trace set, or source set, or path\n");
        return -EINVAL;
    }

    ts_result = calloc(1, sizeof(struct trace_set));
    if(!ts_result)
    {
        err("Trace set allocation failed\n");
        return -ENOMEM;
    }

    ts_result->ts_file = fopen(path, "wb+");
    if(!ts_result->ts_file)
    {
        err("Unable to open trace set at %s: %s\n", path, strerror(errno));
        ret = -errno;
        goto __free_ts_result;
    }

    // this new trace set inherits these headers
    ts_result->num_samples = from->num_samples;
    ts_result->num_traces = from->num_traces;
    ts_result->num_traces_written = 0;
    ts_result->prev_next_trace = 0;
    ts_result->indices_processing = NULL;

    ts_result->input_offs = from->input_offs;
    ts_result->input_len = from->input_len;
    ts_result->output_offs = from->output_offs;
    ts_result->output_len = from->output_len;
    ts_result->key_offs = from->key_offs;
    ts_result->key_len = from->key_len;

    ts_result->title_size = from->title_size;
    ts_result->data_size = from->data_size;
    ts_result->datatype = from->datatype;
    ts_result->yscale = from->yscale;

    ts_result->set_id = __atomic_fetch_add(&gbl_set_index, 1, __ATOMIC_RELAXED);
    debug("Creating new trace set with ID %li\n", ts_result->set_id);

    ts_result->cache = NULL;
    ts_result->prev = from;
    ts_result->tfm = NULL;

    ret = write_default_headers(ts_result);
    if(ret < 0)
    {
        err("Failed to write default headers\n");
        goto __close_ts_file;
    }

    ret = write_inherited_headers(ts_result);
    if(ret < 0)
    {
        err("Failed to write inherited headers\n");
        goto __close_ts_file;
    }

    ret = fseek(ts_result->ts_file, 0, SEEK_SET);
    if(ret < 0)
    {
        err("Failed to seek trace set file to beginning\n");
        goto __close_ts_file;
    }

    ret = read_headers(ts_result);
    if(ret < 0)
    {
        err("Failed to read recently written headers\n");
        goto __close_ts_file;
    }

    ret = sem_init(&ts_result->file_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_headers;
    }

    *ts = ts_result;
    return 0;

__free_headers:
    free(ts_result->headers);

__close_ts_file:
    fclose(ts_result->ts_file);

__free_ts_result:
    free(ts_result);
    *ts = NULL;
    return ret;
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
    ts_result->set_id = __atomic_fetch_add(&gbl_set_index, 1, __ATOMIC_RELAXED);
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

    // todo changes for new trace files
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
