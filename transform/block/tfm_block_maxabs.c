#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <math.h>

#define TFM_DATA(tfm)   ((struct tfm_block_maxabs *) (tfm)->tfm_data)

struct tfm_block_maxabs
{
    bool per_sample;
    int blocksize;
};

int __tfm_block_maxabs_init(struct trace_set *ts)
{
    struct tfm_block_maxabs *tfm = TFM_DATA(ts->tfm);

    if(tfm->per_sample)
    {
        ts->num_samples = ts->prev->num_samples;
        ts->num_traces = ts_num_traces(ts->prev) / tfm->blocksize;
    }
    else
    {
        ts->num_samples = ts_num_traces(ts->prev) / tfm->blocksize;
        ts->num_traces = 1;
    }

    ts->input_offs = ts->input_len =
    ts->output_offs = ts->output_len =
    ts->key_offs = ts->key_len = 0;

    ts->title_size = 0;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1;
    return 0;
}

int __tfm_block_maxabs_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_block_maxabs_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __tfm_block_maxabs_title(struct trace *t, char **title)
{
    *title = NULL;
    return 0;
}

int __tfm_block_maxabs_data(struct trace *t, uint8_t **data)
{
    *data = NULL;
    return 0;
}

int __tfm_block_maxabs_samples(struct trace *t, float **samples)
{
    int i, j, ret;
    float *result = NULL, *curr_samples;
    int indices[ts_num_samples(t->owner)];

    struct trace *curr;
    struct tfm_block_maxabs *tfm = TFM_DATA(t->owner->tfm);

    if(tfm->per_sample)
    {
        result = calloc(ts_num_samples(t->owner), sizeof(float));
        if(!result)
        {
            err("Failed to allocate result array\n");
            return -ENOMEM;
        }

        for(i = 0; i < tfm->blocksize; i++)
        {
            ret = trace_get(t->owner->prev, &curr, TRACE_IDX(t) * tfm->blocksize + i, false);
            if(ret < 0)
            {
                err("Failed to get trace from previous trace set\n");
                goto __fail_free_result;
            }

            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
            {
                err("Failed to get samples from previous trace\n");
                goto __fail_free_trace;
            }

            if(curr_samples)
            {
                for(j = 0; j < ts_num_samples(t->owner); j++)
                {
                    if(fabsf(curr_samples[j]) > result[j])
                    {
                        result[j] = fabsf(curr_samples[j]);
                        indices[j] = i;
                    }
                }
            }

            trace_free(curr);
        }

        if(TRACE_IDX(t) % 16 == 0)
        {
            fprintf(stdout, "%li\t", TRACE_IDX(t));
            for(i = 0; i < ts_num_samples(t->owner); i++)
                fprintf(stdout, "%02X,", indices[i]);
            fprintf(stdout, "\n");
        }

        *samples = result;
        return 0;
    }
    else
    {
        err("Unimplemented\n");
        return -EINVAL;
    }

__fail_free_trace:
    trace_free(curr);

__fail_free_result:
    free(result);
    return ret;
}

void __tfm_block_maxabs_exit(struct trace_set *ts)
{}

void __tfm_block_maxabs_free_title(struct trace *t)
{}

void __tfm_block_maxabs_free_data(struct trace *t)
{}

void __tfm_block_maxabs_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_block_maxabs(struct tfm **tfm, bool per_sample, int blocksize)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_block_maxabs);

    res->tfm_data = calloc(1, sizeof(struct tfm_block_maxabs));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->per_sample = per_sample;
    TFM_DATA(res)->blocksize = blocksize;

    *tfm = res;
    return 0;
}