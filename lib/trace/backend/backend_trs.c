#include "__trace_internal.h"
#include "__backend_internal.h"

#include "trace.h"
#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

int backend_trs_open(struct trace_set *ts)
{
    int ret;
    TRS_ARG(ts)->mode = MODE_READ;

    TRS_ARG(ts)->file = p_fopen(TRS_ARG(ts)->name, "rb");
    if(!TRS_ARG(ts)->file)
    {
        err("Unable to open trace set %s: %s\n", TRS_ARG(ts)->name, strerror(errno));
        return -errno;
    }

    ret = read_headers(ts);
    if(ret < 0)
    {
        err("Failed to initialize trace set headers\n");
        goto __close_ts_file;
    }

    ret = p_sem_create(&TRS_ARG(ts)->file_lock, 1);
    if(ret < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_headers;
    }

    return 0;
__free_headers:
    free_headers(ts);

__close_ts_file:
    p_fclose(TRS_ARG(ts)->file);
    return ret;
}

int backend_trs_create(struct trace_set *ts)
{
    int ret;
    TRS_ARG(ts)->mode = MODE_WRITE;
    TRS_ARG(ts)->num_written = 0;

    TRS_ARG(ts)->file = p_fopen(TRS_ARG(ts)->name, "wb+");
    if(!TRS_ARG(ts)->file)
    {
        err("Failed to open trace set file %s\n", TRS_ARG(ts)->name);
        return -EIO;
    }

    ret = write_default_headers(ts);
    if(ret < 0)
    {
        err("Failed to write default headers\n");
        goto __close_ts_file;
    }

    ret = p_fseek(TRS_ARG(ts)->file, 0, SEEK_SET);
    if(ret < 0)
    {
        err("Failed to seek trace set file to beginning\n");
        goto __close_ts_file;
    }

    ret = read_headers(ts);
    if(ret < 0)
    {
        err("Failed to read recently written headers\n");
        goto __close_ts_file;
    }

    ret = p_sem_create(&TRS_ARG(ts)->file_lock, 1);
    if(ret < 0)
    {
        err("Failed to initialize file lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_headers;
    }

    return 0;
__free_headers:
    free_headers(ts);

__close_ts_file:
    p_fclose(TRS_ARG(ts)->file);
    return ret;
}

int backend_trs_close(struct trace_set *ts)
{
    int ret;
    if(TRS_ARG(ts)->file)
    {
        sem_acquire(&TRS_ARG(ts)->file_lock);
        if(TRS_ARG(ts)->mode == MODE_WRITE)
        {
            ret = finalize_headers(ts);
            if(ret < 0)
            {
                err("Failed to finalize headers in write mode\n")
                return ret;
            }
        }

        p_sem_destroy(&TRS_ARG(ts)->file_lock);
        p_fclose(TRS_ARG(ts)->file);
    }

    if(TRS_ARG(ts)->headers)
        free_headers(ts);

    free(TRS_ARG(ts));
    free(ts->backend);
    return 0;
}

int backend_trs_read(struct trace *t)
{
    int ret, i;
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

    sem_acquire(&TRS_ARG(t->owner)->file_lock);
    ret = p_fseek(TRS_ARG(t->owner)->file,
                  TRS_ARG(t->owner)->trace_start +
                  t->index * TRS_ARG(t->owner)->trace_length,
                  SEEK_SET);
    if(ret)
    {
        err("Failed to seek file to trace position\n");
        ret = -EIO;
        goto __fail_unlock;
    }

    read = p_fread(result_title, 1, t->owner->title_size, TRS_ARG(t->owner)->file);
    if(read != t->owner->title_size)
    {
        err("Failed to read title from file (read %li expecting %li)\n",
            read, t->owner->title_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = p_fread(result_data, 1, t->owner->data_size, TRS_ARG(t->owner)->file);
    if(read != t->owner->data_size)
    {
        err("Failed to read data from file (read %li expecting %li)\n",
            read, t->owner->data_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = p_fread(temp, t->owner->datatype & 0xF, t->owner->num_samples, TRS_ARG(t->owner)->file);
    if(read != t->owner->num_samples)
    {
        err("Failed to read samples from file (read %li expecting %li)\n",
            read, t->owner->num_samples);
        ret = -EIO;
        goto __fail_unlock;
    }

    sem_release(&TRS_ARG(t->owner)->file_lock);

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
    sem_release(&TRS_ARG(t->owner)->file_lock);

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

int backend_trs_write(struct trace *t)
{
    int i, ret;
    size_t written;

    size_t temp_len;
    void *temp = NULL;

    if(!t)
    {
        err("Invalid trace\n");
        return -EINVAL;
    }

    if(TRACE_IDX(t) != TRS_ARG(t->owner)->num_written)
    {
        err("Out-of-order trace sent to write\n");
        return -EINVAL;
    }

    switch(t->owner->datatype)
    {
        case DT_BYTE:
            temp_len = ts_num_samples(t->owner) * sizeof(char);
            temp = calloc(t->owner->num_samples, sizeof(char));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((char *) temp)[i] =
                        (char) (t->samples[i] / t->owner->yscale);
            break;

        case DT_SHORT:
            temp_len = ts_num_samples(t->owner) * sizeof(short);
            temp = calloc(t->owner->num_samples, sizeof(short));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((short *) temp)[i] =
                        (short) (t->samples[i] / t->owner->yscale);
            break;

        case DT_INT:
            temp_len = ts_num_samples(t->owner) * sizeof(int);
            temp = calloc(t->owner->num_samples, sizeof(int));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((int *) temp)[i] =
                        (int) (t->samples[i] / t->owner->yscale);
            break;

        case DT_FLOAT:
            temp_len = ts_num_samples(t->owner) * sizeof(float);
            temp = calloc(t->owner->num_samples, sizeof(float));
            if(!temp) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((float *) temp)[i] =
                        (t->samples[i] / t->owner->yscale);
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

    sem_acquire(&TRS_ARG(t->owner)->file_lock);
    ret = p_fseek(TRS_ARG(t->owner)->file,
                  TRS_ARG(t->owner)->trace_start +
                  t->index * TRS_ARG(t->owner)->trace_length,
                  SEEK_SET);
    if(ret)
    {
        err("Failed to seek to trace set file position\n");
        ret = -errno;
        goto __unlock;
    }

    if(t->title)
    {
        debug("Trace %li writing %li bytes of title\n", TRACE_IDX(t), t->owner->title_size);
        written = p_fwrite(t->title, 1, t->owner->title_size, TRS_ARG(t->owner)->file);
        if(written != t->owner->title_size)
        {
            err("Failed to write all bytes of title to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    if(t->data)
    {
        debug("Trace %li writing %li bytes of data\n", TRACE_IDX(t), t->owner->data_size);
        written = p_fwrite(t->data, 1, t->owner->data_size, TRS_ARG(t->owner)->file);
        if(written != t->owner->data_size)
        {
            err("Failed to write all bytes of data to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    debug("Trace %li writing %li bytes of samples\n", TRACE_IDX(t), temp_len);
    written = p_fwrite(temp, 1, temp_len, TRS_ARG(t->owner)->file);
    if(written != temp_len)
    {
        err("Failed to write all bytes of samples to file\n");
        ret = -EIO;
        goto __free_temp;
    }

    // make number of traces agree
    TRS_ARG(t->owner)->num_written++;
    ret = finalize_headers(t->owner);
    if(ret < 0)
    {
        err("Failed to update headers in trace file\n");
        goto __unlock;
    }

    fflush(TRS_ARG(t->owner)->file);
    ret = 0;
__unlock:
    sem_release(&TRS_ARG(t->owner)->file_lock);

__free_temp:
    free(temp);
    return ret;
}

int create_backend_trs(struct trace_set *ts, const char *name)
{
    struct backend_trs_arg *arg;
    struct backend_intf *res = calloc(1, sizeof(struct backend_intf));
    if(!res)
    {
        err("Failed to allocate backend interface struct\n");
        return -ENOMEM;
    }

    res->open = backend_trs_open;
    res->create = backend_trs_create;
    res->close = backend_trs_close;
    res->read = backend_trs_read;
    res->write = backend_trs_write;

    arg = calloc(1, sizeof(struct backend_trs_arg));
    if(!arg)
    {
        err("Failed to allocate argument for backend\n");
        goto __free_res;
    }

    arg->name = calloc(strlen(name) + 1, sizeof(char));
    if(!arg->name)
    {
        err("Failed to allocate name\n");
        goto __free_arg;
    }

    strcpy(arg->name, name);
    res->arg = arg;
    ts->backend = res;
    return 0;

__free_arg:
    free(arg);

__free_res:
    free(res);
    return -ENOMEM;
}