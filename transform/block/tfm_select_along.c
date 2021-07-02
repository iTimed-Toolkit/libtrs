#include "trace.h"
#include "transform.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <string.h>
#include <errno.h>
#include <math.h>
#include <float.h>

struct tfm_select_along_config
{
    summary_t stat;
    filter_t along;
    filter_param_t param;
};

struct tfm_select_along_block
{
    int count;
    void *cmp_data;

    float val;
    struct trace *t;
};

int tfm_select_along_init(struct trace_set *ts, void *arg)
{
    struct tfm_select_along_config *cfg = arg;
    switch(cfg->along)
    {
        case ALONG_NUM:
        case ALONG_DATA:
            ts->data_size = ts->prev->data_size;
            ts->num_samples = ts->prev->num_samples;
            break;

        default:
            err("Unknown along enum value\n");
            return -EINVAL;
    }

    return 0;
}

int tfm_select_along_exit(struct trace_set *ts, void *arg)
{
    free(arg);
    return 0;
}

int tfm_select_along_initialize(struct trace *t, void **block, void *arg)
{
    int ret;
    struct tfm_select_along_block *new;
    struct tfm_select_along_config *cfg = arg;

    new = calloc(1, sizeof(struct tfm_select_along_block));
    if(!new)
    {
        err("Failed to allocate new block\n");
        return -ENOMEM;
    }

    switch(cfg->along)
    {
        case ALONG_NUM:
            new->count = 0;
            break;

        case ALONG_DATA:
            new->cmp_data = calloc(1, t->owner->data_size);
            if(!new->cmp_data)
            {
                err("Failed to allocate comparison data for new block\n");
                ret = -ENOMEM;
                goto __free_new;
            }

            memcpy(new->cmp_data, t->data, t->owner->data_size);
            break;

        default:
            err("Unknown along enum value\n");
            return -EINVAL;
    }

    new->val = -FLT_MAX;
    *block = new;
    return 0;

__free_new:
    free(new);
    return ret;
}

bool tfm_select_along_interesting(struct trace *t, void *arg)
{
    struct tfm_select_along_config *cfg = arg;
    switch(cfg->along)
    {
        case ALONG_NUM:
            return (t->samples);

        case ALONG_DATA:
            return (t->data && t->samples);

        default:
            err("Unknown along enum value\n");
            return false;
    }
}

bool tfm_select_along_matches(struct trace *t, void *block, void *arg)
{
    struct tfm_select_along_block *blk = block;
    struct tfm_select_along_config *cfg = arg;

    switch(cfg->along)
    {
        case ALONG_NUM:
            return blk->count < cfg->param.num;

        case ALONG_DATA:
            return memcmp(blk->cmp_data, t->data, t->owner->data_size) == 0;

        default:
            err("Unknown along enum value\n");
            return false;
    }
}

int tfm_select_along_accumulate(struct trace *t, void *block, void *arg)
{
    int i;
    float val = -FLT_MAX;

    struct tfm_select_along_block *blk = block;
    struct tfm_select_along_config *cfg = arg;

    if(tfm_select_along_matches(t, block, arg))
    {
        switch(cfg->along)
        {
            case ALONG_NUM:
                blk->count++;
            case ALONG_DATA:
                break;

            default:
                err("Unknown along enum value\n");
                return -EINVAL;
        }
    }
    else
    {
        err("Accumulate called with incorrect block\n");
        return -EINVAL;
    }

    switch(cfg->stat)
    {
        case SUMMARY_MAX:
            for(i = 0; i < t->owner->num_samples; i++)
            {
                if(fabsf(t->samples[i]) > val)
                    val = fabsf(t->samples[i]);
            }

            if(val > blk->val)
            {
                if(blk->t)
                {
                    blk->t->owner = NULL;
                    trace_free_memory(blk->t);
                }

                trace_copy(&blk->t, t);
                blk->val = val;
            }

            break;

        case SUMMARY_MIN:
        case SUMMARY_AVG:
        case SUMMARY_DEV:
            err("Unimplemented\n");
            return -EINVAL;
    }

    return 0;
}

int tfm_select_along_finalize(struct trace *t, void *block, void *arg)
{
    int ret;
    struct tfm_select_along_block *blk = block;

    if(!t)
    {
        err("Finalize called for invalid trace\n");
        return -EINVAL;
    }

    // to ensure copy functions work
    blk->t->owner = t->owner;

    ret = copy_title(t, blk->t);
    if(ret >= 0)
        ret = copy_data(t, blk->t);
    if(ret >= 0)
        ret = copy_samples(t, blk->t);

    if(ret < 0)
    {
        err("Failed to copy something\n");
        return ret;
    }

    if(blk->cmp_data)
        free(blk->cmp_data);

    blk->t->owner = NULL;
    trace_free_memory(blk->t);
    free(blk);
    return 0;
}

int tfm_select_along(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param)
{
    int ret;
    struct block_args block_args = {
            .consumer_init = tfm_select_along_init,
            .consumer_exit = tfm_select_along_exit,
            .consumer_init_waiter = NULL,

            .initialize = tfm_select_along_initialize,
            .trace_interesting = tfm_select_along_interesting,
            .trace_matches = tfm_select_along_matches,
            .accumulate = tfm_select_along_accumulate,
            .finalize = tfm_select_along_finalize,

            .criteria = DONE_LISTLEN
    };

    struct tfm_select_along_config *cfg;
    cfg = calloc(1, sizeof(struct tfm_select_along_config));
    if(!cfg)
    {
        err("Failed to allocate config struct\n");
        return -ENOMEM;
    }

    cfg->stat = stat;
    cfg->along = along;
    cfg->param = param;
    block_args.arg = cfg;

    ret = tfm_block(tfm, &block_args);
    if(ret < 0)
    {
        err("Failed to initialize generic block transform\n");
        return ret;
    }

    return 0;
}