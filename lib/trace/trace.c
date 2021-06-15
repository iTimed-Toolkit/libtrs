#include "trace.h"
#include "__trace_internal.h"
#include "__tfm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
        err("Index %li out of bounds for trace set\n", index);
        return -EINVAL;
    }

    debug("Getting trace %li from trace set %li\n", index, ts->set_id);
    if(ts->cache)
    {
        debug("Looking up trace %li in cache\n", index);
        ret = tc_lookup(ts->cache, index, &t_result,
                        COHESIVE_CACHES ? true : false);
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
        ret = read_trace_from_file(t_result);
        if(ret < 0)
        {
            err("Failed to read trace from file\n");
            goto __fail;
        }
    }

    if(cache_missed)
    {
        debug("Storing trace %li in the cache\n", index);
        ret = tc_store(ts->cache, index, t_result,
                       COHESIVE_CACHES ? true : false);
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

int read_trace_from_file(struct trace *t)
{
    int ret = 0, i;
    size_t read;

    char *result_title = NULL;
    uint8_t *result_data = NULL;
    float *result_samples = NULL;
    void *temp = NULL;

    if(t->owner->title_size)
    {
        result_title = calloc(1, t->owner->title_size);
        if(!result_title)
        {
            err("Failed to allocate memory for trace title\n");
            ret = -ENOMEM;
            goto __fail;
        }
    }

    if(t->owner->data_size)
    {
        result_data = calloc(1, t->owner->data_size);
        if(!result_data)
        {
            err("Failed to allocate memory for trace data\n");
            ret = -ENOMEM;
            goto __fail;
        }
    }

    if(t->owner->num_samples)
    {
        temp = calloc(t->owner->datatype & 0xF, t->owner->num_samples);
        if(!temp)
        {
            err("Failed to allocate memory for temp sample buffer\n");
            ret = -ENOMEM;
            goto __fail;
        }

        result_samples = calloc(sizeof(float), t->owner->num_samples);
        if(!result_samples)
        {
            err("Failed to allocate memory for sample buffer\n");
            ret = -ENOMEM;
            goto __fail;
        }
    }

    sem_acquire(&t->owner->file_lock);

    ret = fseek(t->owner->ts_file, t->start_offset, SEEK_SET);
    if(ret)
    {
        err("Failed to seek file to trace position\n");
        ret = -EIO;
        goto __fail_unlock;
    }

    read = fread(result_title, 1, t->owner->title_size, t->owner->ts_file);
    if(read != t->owner->title_size)
    {
        err("Failed to read title from file (read %li expecting %li)\n",
            read, t->owner->title_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = fread(result_data, 1, t->owner->data_size, t->owner->ts_file);
    if(read != t->owner->data_size)
    {
        err("Failed to read data from file (read %li expecting %li)\n",
            read, t->owner->data_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = fread(temp, t->owner->datatype & 0xF, t->owner->num_samples, t->owner->ts_file);
    if(read != t->owner->num_samples)
    {
        err("Failed to read samples from file (read %li expecting %li)\n",
            read, t->owner->num_samples);
        ret = -EIO;
        goto __fail_unlock;
    }

    sem_release(&t->owner->file_lock);

    // expand samples
    switch(t->owner->datatype)
    {
        case DT_BYTE:
            for(i = 0; i < t->owner->num_samples; i++)
                result_samples[i] = t->owner->yscale * (float) ((char *) temp)[i];
            break;

        case DT_SHORT:
            for(i = 0; i < t->owner->num_samples; i++)
                result_samples[i] = t->owner->yscale * (float) ((short *) temp)[i];
            break;

        case DT_INT:
            for(i = 0; i < t->owner->num_samples; i++)
                result_samples[i] = t->owner->yscale * (float) ((int *) temp)[i];
            break;

        case DT_FLOAT:
            for(i = 0; i < t->owner->num_samples; i++)
                result_samples[i] = t->owner->yscale * ((float *) temp)[i];
            break;

        case DT_NONE:
            err("Invalid trace data type: %i\n", t->owner->datatype);
            goto __fail;
    }

    free(temp);

    t->title = result_title;
    t->data = result_data;
    t->samples = result_samples;
    return 0;

__fail_unlock:
    sem_release(&t->owner->file_lock);

__fail:
    if(result_title)
        free(result_title);

    if(result_data)
        free(result_data);

    if(temp)
        free(temp);

    if(result_samples)
        free(result_samples);

    return ret;
}

//int trace_title(struct trace *t, char **title)
//{
//    if(!t || !title)
//    {
//        err("Invalid trace or title pointer\n");
//        return -EINVAL;
//    }
//
//    *title = t->title;
//    return 0;
//}
//
//int trace_data(struct trace *t, uint8_t **data)
//{
//    if(!t || !data)
//    {
//        err("Invalid trace or data pointer\n");
//        return -EINVAL;
//    }
//
//    *data = t->data;
//    return 0;
//}
//
//int trace_samples(struct trace *t, float **samples)
//{
//    if(!t || !samples)
//    {
//        err("Invalid trace or sample pointer\n");
//        return -EINVAL;
//    }
//
//    *samples = t->samples;
//    return 0;
//}
