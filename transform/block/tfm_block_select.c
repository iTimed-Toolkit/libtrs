#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <float.h>

#define TFM_DATA(tfm)   ((struct tfm_block_select *) (tfm)->tfm_data)

struct tfm_block_select
{
    int blocksize;
    block_t block;
};


int __tfm_block_select_init(struct trace_set *ts)
{
    struct tfm_block_select *tfm = TFM_DATA(ts->tfm);

    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces / tfm->blocksize;

    ts->input_offs = ts->prev->input_offs;
    ts->input_len = ts->prev->input_len;
    ts->output_offs = ts->prev->output_offs;
    ts->output_len = ts->prev->output_len;
    ts->key_offs = ts->prev->key_offs;
    ts->key_len = ts->prev->key_len;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_block_select_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_block_select_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __find_specified(struct trace *t, struct trace **res, struct tfm_block_select *tfm)
{
    int i, j, ret;
    bool replace;
    struct trace *curr, *best = NULL;
    float *curr_samples, best_f;

    switch(tfm->block)
    {
        case BLOCK_MAX:
        case BLOCK_MAXABS:
            best_f = 0;
            break;

        case BLOCK_MIN:
        case BLOCK_MINABS:
            best_f = FLT_MAX;
            break;

        default:
            err("Unrecognized block type\n");
            return -EINVAL;
    }

    for(i = 0; i < tfm->blocksize; i++)
    {
        ret = trace_get(t->owner->prev, &curr, TRACE_IDX(t) * tfm->blocksize + i, false);
        if(ret < 0)
        {
            err("Failed to get trace from previous trace set\n");
            if(best)
                trace_free(best);

            return ret;
        }

        ret = trace_samples(curr, &curr_samples);
        if(ret < 0)
        {
            err("Failed to get samples from previous trace\n");
            trace_free(curr);

            if(best)
                trace_free(best);

            return ret;
        }

        replace = false;
        if(curr_samples)
        {
            for(j = 0; j < ts_num_samples(curr->owner); j++)
            {
                switch(tfm->block)
                {
                    case BLOCK_MAX:
                        if(curr_samples[j] > best_f)
                        {
                            replace = true;
                            best_f = curr_samples[j];
                        }
                        break;

                    case BLOCK_MIN:
                        if(curr_samples[j] < best_f)
                        {
                            replace = true;
                            best_f = curr_samples[j];
                        }
                        break;

                    case BLOCK_MAXABS:
                        if(fabsf(curr_samples[j]) > best_f)
                        {
                            replace = true;
                            best_f = fabsf(curr_samples[j]);
                        }
                        break;

                    case BLOCK_MINABS:
                        if(fabsf(curr_samples[j]) < best_f)
                        {
                            replace = true;
                            best_f = fabsf(curr_samples[j]);
                        }
                        break;
                }

                if(replace)
                    break;
            }
        }

        if(replace)
        {
            if(best)
                trace_free(best);
            best = curr;
        }
        else trace_free(curr);
    }

    *res = best;
    return 0;
}

int __tfm_block_select_title(struct trace *t, char **title)
{
    int ret;
    char *res, *best_title = NULL;
    struct trace *best;
    struct tfm_block_select *tfm = TFM_DATA(t->owner->tfm);

    ret = __find_specified(t, &best, tfm);
    if(ret < 0)
    {
        err("Failed to find specified trace\n");
        return ret;
    }

    ret = trace_title(best, &best_title);
    if(ret < 0)
    {
        err("Failed to get title from best trace\n");
        goto __free_best;
    }

    if(best_title)
    {
        res = calloc(t->owner->title_size, sizeof(char));
        if(!res)
        {
            err("Failed to allocate result buffer\n");
            goto __free_best;
        }

        memcpy(res, best_title, t->owner->title_size * sizeof(char));
        *title = res;
    }
    else *title = NULL;

__free_best:
    trace_free(best);
    return ret;
}

int __tfm_block_select_data(struct trace *t, uint8_t **data)
{
    int ret;
    uint8_t *res, *best_data = NULL;
    struct trace *best;
    struct tfm_block_select *tfm = TFM_DATA(t->owner->tfm);

    ret = __find_specified(t, &best, tfm);
    if(ret < 0)
    {
        err("Failed to find specified trace\n");
        return ret;
    }

    ret = trace_data_all(best, &best_data);
    if(ret < 0)
    {
        err("Failed to get data from best trace\n");
        goto __free_best;
    }

    if(best_data)
    {
        res = calloc(t->owner->data_size, sizeof(uint8_t));
        if(!res)
        {
            err("Failed to allocate result buffer\n");
            goto __free_best;
        }

        memcpy(res, best_data, t->owner->data_size * sizeof(uint8_t));
        *data = res;
    }
    else *data = NULL;

__free_best:
    trace_free(best);
    return ret;
}

int __tfm_block_select_samples(struct trace *t, float **samples)
{
    int ret;
    float *res, *best_samples = NULL;
    struct trace *best;
    struct tfm_block_select *tfm = TFM_DATA(t->owner->tfm);

    ret = __find_specified(t, &best, tfm);
    if(ret < 0)
    {
        err("Failed to find specified trace\n");
        return ret;
    }

    ret = trace_samples(best, &best_samples);
    if(ret < 0)
    {
        err("Failed to get samples from best trace\n");
        goto __free_best;
    }

    if(best_samples)
    {
        res = calloc(t->owner->num_samples, sizeof(float));
        if(!res)
        {
            err("Failed to allocate result buffer\n");
            goto __free_best;
        }

        memcpy(res, best_samples, t->owner->num_samples * sizeof(float));
        *samples = res;
    }
    else *samples = NULL;

__free_best:
    trace_free(best);
    return ret;
}

void __tfm_block_select_exit(struct trace_set *ts)
{}

void __tfm_block_select_free_title(struct trace *t)
{
    free(t->buffered_title);
}

void __tfm_block_select_free_data(struct trace *t)
{
    free(t->buffered_data);
}

void __tfm_block_select_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_block_select(struct tfm **tfm, int blocksize, block_t block)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_block_select);

    res->tfm_data = calloc(1, sizeof(struct tfm_block_select));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->blocksize = blocksize;
    TFM_DATA(res)->block = block;

    *tfm = res;
    return 0;
}