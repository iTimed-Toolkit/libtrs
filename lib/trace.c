#include "libtrs.h"
#include "__libtrs_internal.h"
#include "__tfm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_IDX(t)  (((t)->start_offset - (t)->owner->trace_start)  / \
                            (t)->owner->trace_length)


int trace_free_memory(struct trace *t)
{
    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    if(t->owner->prev && t->owner->tfm)
    {
        if(t->buffered_title)
            t->owner->tfm->free_title(t);

        if(t->buffered_data)
            t->owner->tfm->free_data(t);

        if(t->buffered_samples)
            t->owner->tfm->free_samples(t);
    }
    else
    {
        if(t->buffered_title)
            free(t->buffered_title);

        if(t->buffered_data)
            free(t->buffered_data);

        if(t->buffered_samples)
            free(t->buffered_samples);
    }

    free(t);
    return 0;
}

int index_order(void *node1, void *node2)
{
    size_t index1 = *((size_t *) node1);
    size_t index2 = *((size_t *) node2);

    return (int) index2 - (int) index1;
}

void index_print(void *data)
{
    fprintf(stderr, "%p, ", data);
    fflush(stderr);
    size_t index = *((size_t *) data);
    fprintf(stderr, "%li\n", index);
}

int __append_trace_to_file(struct trace *t)
{
    int i;
    int ret;
    size_t written;

    size_t temp_len;
    void *temp = NULL;

    warn("Appending trace %li to file\n", TRACE_IDX(t));

    ret = fseek(t->owner->ts_file, t->start_offset, SEEK_SET);
    if(ret < 0)
    {
        err("Failed to seek to end of trace set file\n");
        return -EIO;
    }

    // todo avx accelerate?
    switch(t->owner->datatype)
    {
        case DT_BYTE:
            temp_len = ts_num_samples(t->owner) * sizeof(char);
            temp = calloc(t->owner->num_samples, sizeof(char));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((char *) temp)[i] = (char) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_SHORT:
            temp_len = ts_num_samples(t->owner) * sizeof(short);
            temp = calloc(t->owner->num_samples, sizeof(short));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((short *) temp)[i] = (short) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_INT:
            temp_len = ts_num_samples(t->owner) * sizeof(int);
            temp = calloc(t->owner->num_samples, sizeof(int));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((int *) temp)[i] = (int) (t->buffered_samples[i] / t->owner->yscale);
            break;

        case DT_FLOAT:
            temp_len = ts_num_samples(t->owner) * sizeof(float);
            temp = calloc(t->owner->num_samples, sizeof(float));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((float *) temp)[i] = t->buffered_samples[i] / t->owner->yscale;
            break;

        case DT_NONE:
        default:
            err("Bad trace set datatype %i\n", t->owner->datatype);
            return -EINVAL;
    }

    if(!temp)
    {
        err("Failed to allocate temporary memory\n");
        return -ENOMEM;
    }

    if(t->buffered_title)
    {
        written = fwrite(t->buffered_title, 1, t->owner->title_size, t->owner->ts_file);
        if(written != t->owner->title_size)
        {
            err("Failed to write all bytes of title to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    if(t->buffered_data)
    {
        written = fwrite(t->buffered_data, 1, t->owner->data_size, t->owner->ts_file);
        if(written != t->owner->data_size)
        {
            err("Failed to write all bytes of data to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    written = fwrite(temp, 1, temp_len, t->owner->ts_file);
    if(written != temp_len)
    {
        err("Failed to write all bytes of samples to file\n");
        ret = -EIO;
        goto __free_temp;
    }

    fflush(t->owner->ts_file);

    ret = 0;
__free_temp:
    free(temp);
    return ret;

}

int __trace_render_to_index(struct trace_set *ts, struct trace **t, size_t index)
{
    int ret;

    size_t prev_index;
    struct list *node;
    struct trace *t_prev = NULL, *t_result = NULL;

    while(1)
    {
        ret = sem_wait(&ts->file_lock);
        if(ret < 0)
        {
            err("Failed to wait on trace set file lock\n");
            return -errno;
        }

        if(index < ts->num_traces_written)
        {
            debug("Index %li < written %li, exiting\n", index, ts->num_traces_written);
            ret = sem_post(&ts->file_lock);
            if(ret < 0)
            {
                err("Failed to post to trace set file lock\n");
                return -errno;
            }

            break;
        }

        // get a new index to work on
        prev_index = ts->prev_next_trace;
        ts->prev_next_trace++;

        list_create_node(&node, &prev_index);
        list_link_single(&ts->indices_processing, node, index_order);

        ret = sem_post(&ts->file_lock);
        if(ret < 0)
        {
            err("Failed to post to trace set file lock\n");
            return -errno;
        }

        debug("Checking prev_index %li\n", prev_index);
        if(prev_index >= ts_num_traces(ts->prev))
        {
            err("Index %li out of bounds for previous trace set\n", prev_index);
            return -EINVAL;
        }

        // potentially takes a long time, depending on transformation chain
        ret = trace_get(ts->prev, &t_prev, prev_index, true);
        if(ret < 0)
        {
            err("Failed to get trace from previous trace set\n");
            return ret;
        }

        // this is not a valid trace for this trace set
        if(!t_prev->buffered_samples ||
           (ts->prev->title_size != 0 && !t_prev->buffered_title) ||
           (ts->prev->data_size != 0 && !t_prev->buffered_data))
        {
            debug("prev_index %li not a valid index\n", prev_index);

            // release this node
            ret = sem_wait(&ts->file_lock);
            if(ret < 0)
            {
                err("Failed to wait on trace set file lock\n");
                ret = -errno;
                goto __fail_free_prev;
            }

            list_unlink_single(&ts->indices_processing, node);
            list_free_node(node);

            ret = sem_post(&ts->file_lock);
            if(ret < 0)
            {
                err("Failed to post to trace set file lock\n");
                ret = -errno;
                goto __fail_free_prev;
            }

            trace_free(t_prev);
            continue;
        }

        debug("prev_index %li is a valid index, appending\n", prev_index);

        t_result = calloc(1, sizeof(struct trace));
        if(!t_result)
        {
            err("Failed to allocate memory for trace\n");
            ret = -ENOMEM;
            goto __fail_free_prev;
        }

        t_result->owner = ts;
        t_result->buffered_title = t_prev->buffered_title;
        t_result->buffered_data = t_prev->buffered_data;
        t_result->buffered_samples = t_prev->buffered_samples;

        t_result->buffered_title = calloc(ts->title_size, sizeof(char));
        if(!t_result->buffered_title)
        {
            err("Failed to allocate memory for new trace title\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        t_result->buffered_data = calloc(ts->data_size, sizeof(uint8_t));
        if(!t_result->buffered_data)
        {
            err("Failed to allocate memory for new trace data\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        t_result->buffered_samples = calloc(sizeof(float), ts->num_samples);
        if(!t_result->buffered_samples)
        {
            err("Failed to allocate memory for new trace samples\n");
            ret = -ENOMEM;
            goto __fail_free_result;
        }

        memcpy(t_result->buffered_title, t_prev->buffered_title, ts->title_size * sizeof(char));
        memcpy(t_result->buffered_data, t_prev->buffered_data, ts->data_size * sizeof(uint8_t));
        memcpy(t_result->buffered_samples, t_prev->buffered_samples, ts->num_samples * sizeof(float));

        // wait for our turn to write
        while(1)
        {
            ret = sem_wait(&ts->file_lock);
            if(ret < 0)
            {
                err("Failed to wait on trace set file lock\n");
                ret = -errno;
                goto __fail_free_result;
            }

            if(list_lookup_single(ts->indices_processing, node) == 0)
                break;
            else
            {
                ret = sem_post(&ts->file_lock);
                if(ret < 0)
                {
                    err("Failed to post to trace set file lock\n");
                    ret = -errno;
                    goto __fail_free_result;
                }
            }
        }

        t_result->start_offset = ts->trace_start + ts->num_traces_written * ts->trace_length;
        if(ts->cache)
        {
            ret = tc_store(ts, ts->num_traces_written, t_result);
            if(ret < 0)
            {
                err("Failed to store result trace in cache\n");
                ret = -errno;
                goto __fail_free_result;
            }
        }

        __append_trace_to_file(t_result);
        ts->num_traces_written++;

        if(ts->num_traces_written - 1 == index)
            *t = t_result;
        else
            trace_free(t_result);

        list_unlink_single(&ts->indices_processing, node);
        list_free_node(node);

        ret = sem_post(&ts->file_lock);
        if(ret < 0)
        {
            err("Failed to post to trace set file lock\n");
            ret = -errno;
            goto __fail_free_prev;
        }

        trace_free(t_prev);
    }

    return 0;

__fail_free_result:
    trace_free_memory(t_result);
    free(t_result);

__fail_free_prev:
    trace_free(t_prev);

    return ret;
}

int trace_get(struct trace_set *ts, struct trace **t, size_t index, bool prebuffer)
{
    int ret;
    struct trace *t_result;
    size_t written;

    bool cache_missed = false;

    if(!ts || !t)
    {
        err("Invalid trace set or trace\n");
        return -EINVAL;
    }

    if(ts->prev && ts->ts_file)
    {
        __atomic_load(&ts->num_traces_written, &written, __ATOMIC_RELAXED);

        // this is a newly created trace set, need to sequentially access
        if(index >= written)
        {
            debug("Rendering trace set %li to index %li\n", ts->set_id, index);

            *t = NULL;
            ret = __trace_render_to_index(ts, t, index);
            if(ret < 0)
            {
                err("Failed to render trace set up to index %li\n", index);
                return ret;
            }

            // this thread got the correct index, and went ahead and stored the result for us
            if(*t)
            {
                debug("Already got correct trace\n");
                return 0;
            }

            debug("Getting correct trace from cache or file\n");
        }
    }
    else
    {
        if(index >= ts->num_traces)
        {
            err("Index %li out of bounds for trace set\n", index);
            return -EINVAL;
        }
    }

    debug("Getting trace %li from trace set %li\n", index, ts->set_id);
    if(ts->cache)
    {
        debug("Looking up in cache\n");
        ret = tc_lookup(ts, index, &t_result);
        if(ret < 0)
        {
            err("Failed to lookup trace in cache\n");
            return ret;
        }

        if(t_result) goto __out;
        else cache_missed = true;

        debug("Trace %li not found in cache\n", index);
    }

    t_result = calloc(1, sizeof(struct trace));
    if(!t_result)
    {
        err("Failed to allocate memory for trace\n");
        return -ENOMEM;
    }

    t_result->owner = ts;
    t_result->start_offset = ts->trace_start + index * ts->trace_length;
    t_result->buffered_title = NULL;
    t_result->buffered_data = NULL;
    t_result->buffered_samples = NULL;

    if(prebuffer || cache_missed)
    {
        debug("Prebuffering trace %li\n", index);
        ret = trace_title(t_result, &t_result->buffered_title);
        if(ret < 0)
            goto __fail;

        ret = trace_data_all(t_result, &t_result->buffered_data);
        if(ret < 0)
            goto __fail;

        ret = trace_samples(t_result, &t_result->buffered_samples);
        if(ret < 0)
            goto __fail;
    }

    if(cache_missed)
    {
        debug("Storing trace %li in the cache\n", index);
        ret = tc_store(ts, index, t_result);
        if(ret < 0)
        {
            err("Failed to store result trace in cache\n");
            goto __fail;
        }
    }

__out:
    *t = t_result;
    return 0;

__fail:
    trace_free_memory(t_result);
    *t = NULL;
    return ret;
}

int trace_free(struct trace *t)
{
    if(t->owner->cache)
    {
        debug("Dereferencing trace in cache\n");
        // this will cause a free if it needs to
        return tc_deref(t->owner,
                        (t->start_offset - t->owner->trace_start) /
                        t->owner->trace_length,
                        t);
    }
    else
    {
        debug("Freeing trace memory\n");
        return trace_free_memory(t);
    }
}

int trace_title(struct trace *t, char **title)
{
    int stat;
    size_t ret;
    char *result;

    if(!t || !title)
    {
        err("Invalid trace or title pointer\n");
        return -EINVAL;
    }

    if(t->buffered_title)
    {
        *title = t->buffered_title;
        return 0;
    }

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->title(t, &result);
        if(stat < 0)
        {
            err("Failed to get title from transformation\n");
            goto __fail;
        }
    }
    else
    {
        result = calloc(1, t->owner->title_size);
        if(!result)
        {
            err("Failed to allocate memory for trace title\n");
            return -ENOMEM;
        }

        ret = sem_wait(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to wait on trace set file lock\n");
            goto __sem_fail;
        }

        stat = fseek(t->owner->ts_file, t->start_offset, SEEK_SET);
        if(stat)
        {
            err("Failed to seek file to title position\n");
            stat = -EIO;
            goto __free_result;
        }

        ret = fread(result, 1, t->owner->title_size, t->owner->ts_file);
        if(ret != t->owner->title_size)
        {
            err("Failed to read title from file\n");
            stat = -EIO;
            goto __free_result;
        }

        ret = sem_post(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to post to trace set file lock\n");
            goto __sem_fail;
        }
    }

    t->buffered_title = result;
    *title = result;
    return 0;

__sem_fail:
    stat = -errno;

__free_result:
    free(result);

__fail:
    return stat;
}

int __trace_buffer_data(struct trace *t)
{
    int stat;
    size_t ret;
    uint8_t *result;

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->data(t, &result);
        if(stat < 0)
        {
            err("Failed to get data from transformation\n");
            goto __fail;
        }
    }
    else
    {
        result = calloc(1, t->owner->data_size);
        if(!result)
        {
            err("Failed to allocate memory for trace data\n");
            return -ENOMEM;
        }

        ret = sem_wait(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to wait on trace set file lock\n");
            goto __sem_fail;
        }

        stat = fseek(t->owner->ts_file, t->start_offset + t->owner->title_size, SEEK_SET);
        if(stat)
        {
            err("Failed to seek file to data position\n");
            stat = -EIO;
            goto __free_result;
        }

        ret = fread(result, 1, t->owner->data_size, t->owner->ts_file);
        if(ret != t->owner->data_size)
        {
            err("Failed to read data from file\n");
            stat = -EIO;
            goto __free_result;
        }

        ret = sem_post(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to post to trace set file lock\n");
            goto __sem_fail;
        }
    }

    t->buffered_data = result;
    return 0;

__sem_fail:
    stat = -errno;

__free_result:
    free(result);

__fail:
    return stat;
}

int __trace_data_generic(struct trace *t, uint8_t **data,
                         size_t offs, size_t len)
{
    int stat;
    if(t->buffered_data)
    {
        *data = &t->buffered_data[offs];
        return len;
    }

    if(offs == -1 || len == -1)
    {
        *data = NULL;
        return 0;
    }

    stat = __trace_buffer_data(t);
    if(stat < 0)
    {
        err("Failed to buffer trace data\n");
        *data = NULL;
        return stat;
    }

    *data = &t->buffered_data[offs];
    return len;
}

int trace_data_all(struct trace *t, uint8_t **data)
{
    if(!t || !data)
    {
        err("Invalid trace or data pointer\n");
        return -EINVAL;
    }

    return __trace_data_generic(t, data, 0, t->owner->data_size);
}

int trace_data_input(struct trace *t, uint8_t **data)
{
    if(!t || !data)
    {
        err("Invalid trace or data pointer\n");
        return -EINVAL;
    }

    return __trace_data_generic(t, data,
                                t->owner->input_offs,
                                t->owner->input_len);
}

int trace_data_output(struct trace *t, uint8_t **data)
{
    if(!t || !data)
    {
        err("Invalid trace or data pointer\n");
        return -EINVAL;
    }

    return __trace_data_generic(t, data,
                                t->owner->output_offs,
                                t->owner->output_len);
}

int trace_data_key(struct trace *t, uint8_t **data)
{
    if(!t || !data)
    {
        err("Invalid trace or data pointer\n");
        return -EINVAL;
    }

    return __trace_data_generic(t, data,
                                t->owner->key_offs,
                                t->owner->key_len);
}

size_t trace_samples(struct trace *t, float **samples)
{
    int stat, i;
    size_t ret;
    void *temp;
    float *result;

    if(!t || !samples)
    {
        err("Invalid trace or sample pointer\n");
        return -EINVAL;
    }

    if(t->buffered_samples)
    {
        *samples = t->buffered_samples;
        return 0;
    }

    if(t->owner->prev && t->owner->tfm)
    {
        stat = t->owner->tfm->samples(t, &result);
        if(stat < 0)
        {
            err("Failed to get samples from transformation\n");
            goto __fail;
        }
    }
    else
    {
        temp = calloc(t->owner->datatype & 0xF, t->owner->num_samples);
        if(!temp)
        {
            err("Failed to allocate memory for temporary calculation buffer\n");
            return -ENOMEM;
        }

        result = calloc(sizeof(float), t->owner->num_samples);
        if(!result)
        {
            err("Failed to allocate memory for sample buffer\n");
            stat = -ENOMEM;
            goto __free_temp;
        }

        ret = sem_wait(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to wait on trace set file lock\n");
            goto __sem_fail;
        }

        stat = fseek(t->owner->ts_file,
                     t->start_offset + t->owner->title_size + t->owner->data_size,
                     SEEK_SET);
        if(stat)
        {
            err("Failed to seek file to sample position\n");
            stat = -EIO;
            goto __free_temp;
        }

        ret = fread(temp, t->owner->datatype & 0xF, t->owner->num_samples, t->owner->ts_file);
        if(ret != t->owner->num_samples)
        {
            err("Failed to read samples from file\n");
            stat = -EIO;
            goto __free_temp;
        }

        ret = sem_post(&t->owner->file_lock);
        if(ret < 0)
        {
            err("Failed to post to trace set file lock\n");
            goto __sem_fail;
        }

        switch(t->owner->datatype)
        {
            case DT_BYTE:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((char *) temp)[i];
                break;

            case DT_SHORT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((short *) temp)[i];
                break;

            case DT_INT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * (float) ((int *) temp)[i];
                break;

            case DT_FLOAT:
                for(i = 0; i < t->owner->num_samples; i++)
                    result[i] = t->owner->yscale * ((float *) temp)[i];
                break;

            case DT_NONE:
                err("Invalid trace data type: %i\n", t->owner->datatype);
                goto __free_result;
        }

        free(temp);
    }

    t->buffered_samples = result;
    *samples = result;
    return 0;

__sem_fail:
    stat = -errno;

__free_result:
    free(result);

__free_temp:
    free(temp);

__fail:
    return stat;
}
