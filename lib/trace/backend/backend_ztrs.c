#include "__trace_internal.h"
#include "__backend_internal.h"

#include "trace.h"

#include <stdlib.h>
#include <zlib.h>

// Reuse these, as they don't change
extern int backend_trs_open(struct trace_set *ts);
extern int backend_trs_create(struct trace_set *ts);
extern int backend_trs_close(struct trace_set *ts);

int __seek_to_trace(struct trace *t, uint32_t *size)
{
    int ret;
    uint32_t last_size, this_size;
    size_t read;

    debug("Reading trace %zu, with %zu traces written\n",
          TRACE_IDX(t), TRS_ARG(t->owner)->num_written);

    while(1)
    {
        debug("Seek for trace %zu at position %zu, pos %li\n", TRACE_IDX(t),
              TRS_ARG(t->owner)->position, p_ftell(TRS_ARG(t->owner)->file));

        read = p_fread(&last_size, sizeof(uint32_t), 1, TRS_ARG(t->owner)->file);
        if(read != 1)
        {
            err("Trace %zu: Failed to read last size from file at position %zu\n",
                TRACE_IDX(t), TRS_ARG(t->owner)->position);
            return -EIO;
        }

        if(TRACE_IDX(t) < TRS_ARG(t->owner)->position)
        {
            ret = p_fseek(TRS_ARG(t->owner)->file,
                          (long) (-1 * (t->owner->title_size + t->owner->data_size +
                                        last_size + 3 * sizeof(uint32_t))), SEEK_CUR);
            if(ret < 0)
            {
                err("Failed to seek file to previous trace\n");
                return -EIO;
            }

            debug("Decreasing position pointer from %zu\n", TRS_ARG(t->owner)->position);
            TRS_ARG(t->owner)->position--;
            continue;
        }

        read = p_fread(&this_size, sizeof(uint32_t), 1, TRS_ARG(t->owner)->file);
        if(read != 1)
        {
            err("Trace %zu: Failed to read last size from file at position %zu\n",
                TRACE_IDX(t), TRS_ARG(t->owner)->position);
            return -EIO;
        }

        if(TRACE_IDX(t) > TRS_ARG(t->owner)->position)
        {
            ret = p_fseek(TRS_ARG(t->owner)->file,
                          (long) (t->owner->title_size + t->owner->data_size + this_size),
                          SEEK_CUR);
            if(ret < 0)
            {
                err("Failed to seek file to next trace\n");
                return -EIO;
            }

            debug("Advancing position pointer from %zu\n", TRS_ARG(t->owner)->position);
            TRS_ARG(t->owner)->position++;
        }
        else
        {
            // We are now at the correct trace position
            *size = this_size;
            return 0;
        }

        debug("Seek for trace %zu changed to @ %li\n", TRACE_IDX(t), p_ftell(TRS_ARG(t->owner)->file));
    }
}

int backend_ztrs_read(struct trace *t)
{
    int ret, i;
    size_t read;
    uint32_t compressed_size;

    char *result_title = NULL;
    uint8_t *result_data = NULL;
    float *result_samples = NULL;
    void *temp = NULL, *compressed = NULL;

    z_stream inf_stream = {
            .zalloc = Z_NULL,
            .zfree = Z_NULL,
            .opaque = Z_NULL
    };

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
    ret = __seek_to_trace(t, &compressed_size);
    if(ret < 0)
    {
        err("Failed to seek to trace\n");
        goto __fail_unlock;
    }

    compressed = calloc(compressed_size, sizeof(uint8_t));
    if(!compressed)
    {
        err("Failed to allocate memory for compressed data\n");
        goto __fail_unlock;
    }

    read = p_fread(result_title, 1, t->owner->title_size, TRS_ARG(t->owner)->file);
    if(read != t->owner->title_size)
    {
        err("Failed to read title from file (read %zu expecting %zu)\n",
            read, t->owner->title_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = p_fread(result_data, 1, t->owner->data_size, TRS_ARG(t->owner)->file);
    if(read != t->owner->data_size)
    {
        err("Failed to read data from file (read %zu expecting %zu)\n",
            read, t->owner->data_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    read = p_fread(compressed, 1, compressed_size, TRS_ARG(t->owner)->file);
    if(read != compressed_size)
    {
        err("Failed to read compressed samples from file (read %zu expecting %i)\n",
            read, compressed_size);
        ret = -EIO;
        goto __fail_unlock;
    }

    // leaving the file pointer in place for the next position
    TRS_ARG(t->owner)->position++;

    debug("Read for trace %zu leaving file @ %li\n", TRACE_IDX(t), p_ftell(TRS_ARG(t->owner)->file));
    sem_release(&TRS_ARG(t->owner)->file_lock);

    // decompress the samples
    inf_stream.avail_in = compressed_size;
    inf_stream.next_in = (Bytef *) compressed;
    inf_stream.avail_out = (t->owner->datatype & 0xF) * t->owner->num_samples;
    inf_stream.next_out = (Bytef *) temp;

    inflateInit(&inf_stream);
    inflate(&inf_stream, Z_NO_FLUSH);
    inflateEnd(&inf_stream);

    if(inf_stream.total_in != compressed_size ||
       inf_stream.total_out != (t->owner->datatype & 0xF) * t->owner->num_samples)
    {
        err("Failed to decompress all data\n");
        ret = -EINVAL;
        goto __fail;
    }

    free(compressed);
    compressed = NULL;

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
    temp = NULL;

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

    if(compressed)
        free(compressed);

    if(temp)
        free(temp);

    if(result_samples)
        free(result_samples);

    return ret;
}

int backend_ztrs_write(struct trace *t)
{
    int i, ret;
    size_t written;
    uint32_t compressed_size, first_size;

    size_t temp_len;
    void *temp = NULL, *compressed = NULL;

    z_stream def_stream = {
            .zalloc = Z_NULL,
            .zfree = Z_NULL,
            .opaque = Z_NULL
    };

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

            compressed = calloc(t->owner->num_samples, sizeof(char));
            if(!compressed) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((char *) temp)[i] =
                        (char) (t->samples[i] / t->owner->yscale);
            break;

        case DT_SHORT:
            temp_len = ts_num_samples(t->owner) * sizeof(short);
            temp = calloc(t->owner->num_samples, sizeof(short));
            if(!temp) break;

            compressed = calloc(t->owner->num_samples, sizeof(short));
            if(!compressed) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((short *) temp)[i] =
                        (short) (t->samples[i] / t->owner->yscale);
            break;

        case DT_INT:
            temp_len = ts_num_samples(t->owner) * sizeof(int);
            temp = calloc(t->owner->num_samples, sizeof(int));
            if(!temp) break;

            compressed = calloc(t->owner->num_samples, sizeof(int));
            if(!compressed) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((int *) temp)[i] =
                        (int) (t->samples[i] / t->owner->yscale);
            break;

        case DT_FLOAT:
            temp_len = ts_num_samples(t->owner) * sizeof(float);
            temp = calloc(t->owner->num_samples, sizeof(float));
            if(!temp) break;

            compressed = calloc(t->owner->num_samples, sizeof(float));
            if(!compressed) break;

            for(i = 0; i < ts_num_samples(t->owner); i++)
                ((float *) temp)[i] =
                        (t->samples[i] / t->owner->yscale);
            break;

        case DT_NONE:
        default:
        err("Bad trace set datatype %i\n", t->owner->datatype);
            return -EINVAL;
    }

    if(!temp || !compressed)
    {
        err("Failed to allocate temporary memory\n");
        return -ENOMEM;
    }

    def_stream.avail_in = temp_len;
    def_stream.next_in = (Bytef *) temp;
    def_stream.avail_out = temp_len;
    def_stream.next_out = (Bytef *) compressed;

    deflateInit(&def_stream, Z_BEST_COMPRESSION);
    deflate(&def_stream, Z_FINISH);
    deflateEnd(&def_stream);

    if(def_stream.total_in != temp_len)
    {
        err("Failed to compress all data\n");
        ret = -EINVAL;
        goto __free_temp;
    }

    debug("Compressed trace %zu by %f\n", TRACE_IDX(t),
          (float) def_stream.total_out / (float) def_stream.total_in);

    compressed_size = def_stream.total_out;
    sem_acquire(&TRS_ARG(t->owner)->file_lock);
    ret = p_fseek(TRS_ARG(t->owner)->file, 0, SEEK_END);
    if(ret)
    {
        err("Failed to seek to end of trace file\n");
        ret = -errno;
        goto __unlock;
    }

    debug("Write for trace %zu starting @ %li\n", TRACE_IDX(t), p_ftell(TRS_ARG(t->owner)->file));

    if(TRS_ARG(t->owner)->num_written == 0)
    {
        first_size = -1;
        written = p_fwrite(&first_size, sizeof(uint32_t), 1, TRS_ARG(t->owner)->file);
        if(written != 1)
        {
            err("Failed to write initial size to file\n");
            ret = -EIO;
            goto __unlock;
        }
    }

    // this size
    written = p_fwrite(&compressed_size, sizeof(uint32_t), 1, TRS_ARG(t->owner)->file);
    if(written != 1)
    {
        err("Failed to write current size to file\n");
        ret = -EIO;
        goto __unlock;
    }

    if(t->title)
    {
        debug("Trace %zu writing %zu bytes of title\n", TRACE_IDX(t), t->owner->title_size);
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
        debug("Trace %zu writing %zu bytes of data\n", TRACE_IDX(t), t->owner->data_size);
        written = p_fwrite(t->data, 1, t->owner->data_size, TRS_ARG(t->owner)->file);
        if(written != t->owner->data_size)
        {
            err("Failed to write all bytes of data to file\n");
            ret = -EIO;
            goto __free_temp;
        }
    }

    debug("Trace %zu writing %i bytes of samples\n", TRACE_IDX(t), compressed_size);
    written = p_fwrite(compressed, 1, compressed_size, TRS_ARG(t->owner)->file);
    if(written != compressed_size)
    {
        err("Failed to write all bytes of samples to file\n");
        ret = -EIO;
        goto __free_temp;
    }

    // prev size
    written = p_fwrite(&compressed_size, sizeof(uint32_t), 1, TRS_ARG(t->owner)->file);
    if(written != 1)
    {
        err("Failed to write current size to file\n");
        ret = -EIO;
        goto __unlock;
    }

    debug("Write for trace %zu ending @ %li\n", TRACE_IDX(t), p_ftell(TRS_ARG(t->owner)->file));

    // make number of traces agree
    TRS_ARG(t->owner)->num_written++;
    ret = finalize_headers(t->owner);
    if(ret < 0)
    {
        err("Failed to update headers in trace file\n");
        goto __unlock;
    }

    TRS_ARG(t->owner)->position = 0;
    p_fflush(TRS_ARG(t->owner)->file);
    ret = 0;
__unlock:
    debug("Write for trace %zu leaving file @ %li\n", TRACE_IDX(t), p_ftell(TRS_ARG(t->owner)->file));
    sem_release(&TRS_ARG(t->owner)->file_lock);

__free_temp:
    free(temp);
    free(compressed);
    return ret;
}

int create_backend_ztrs(struct trace_set *ts, const char *name)
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
    res->read = backend_ztrs_read;
    res->write = backend_ztrs_write;

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
    arg->position = 0;

    res->arg = arg;
    ts->backend = res;
    return 0;

__free_arg:
    free(arg);

__free_res:
    free(res);
    return -ENOMEM;
}