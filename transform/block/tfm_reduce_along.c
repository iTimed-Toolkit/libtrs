#include "transform.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <string.h>
#include <errno.h>

struct tfm_reduce_along_config
{
    summary_t stat;
    filter_t along;
    filter_param_t param;
};

struct tfm_reduce_along_block
{
    int count;
    void *cmp_data;
    struct accumulator *acc;
};

int tfm_reduce_along_init(struct trace_set *ts, void *arg)
{
    struct tfm_reduce_along_config *cfg = arg;
    switch(cfg->along)
    {
        case ALONG_NUM:
            ts->data_size = 0;
            ts->num_samples = ts->prev->num_samples;
            break;

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

int tfm_reduce_along_exit(struct trace_set *ts, void *arg)
{
    free(arg);
    return 0;
}

int tfm_reduce_along_initialize(struct trace *t, void **block, void *arg)
{
    int ret;
    struct tfm_reduce_along_block *new;
    struct tfm_reduce_along_config *cfg = arg;

    new = calloc(1, sizeof(struct tfm_reduce_along_block));
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

    ret = stat_create_single_array(&new->acc, (int) t->owner->num_samples);
    if(ret < 0)
    {
        err("Failed to create new accumulator for block\n");
        goto __free_cmp;
    }

    *block = new;
    return 0;

__free_cmp:
    if(new->cmp_data)
        free(new->cmp_data);

__free_new:
    free(new);
    return ret;
}

bool tfm_reduce_along_interesting(struct trace *t, void *arg)
{
    struct tfm_reduce_along_config *cfg = arg;
    switch(cfg->along)
    {
        case ALONG_NUM:
            return true;

        case ALONG_DATA:
            return (t->data && t->samples);

        default:
            err("Unknown along enum value\n");
            return false;
    }
}

bool tfm_reduce_along_matches(struct trace *t, void *block, void *arg)
{
    struct tfm_reduce_along_block *blk = block;
    struct tfm_reduce_along_config *cfg = arg;

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

int tfm_reduce_along_accumulate(struct trace *t, void *block, void *arg)
{
    int ret;
    struct tfm_reduce_along_block *blk = block;
    struct tfm_reduce_along_config *cfg = arg;

    if(tfm_reduce_along_matches(t, block, arg))
    {
        switch(cfg->along)
        {
            case ALONG_NUM:
                blk->count++;
            case ALONG_DATA:
                ret = stat_accumulate_single_array(blk->acc, t->samples,
                                                   (int) t->owner->num_samples);
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

    if(ret < 0)
    {
        err("Failed to accumulate trace into block\n");
        return ret;
    }

    return 0;
}

int tfm_reduce_along_finalize(struct trace *t, void *block, void *arg)
{
    int ret;
    struct tfm_reduce_along_block *blk = block;
    struct tfm_reduce_along_config *cfg = arg;

    switch(cfg->along)
    {
        case ALONG_NUM:
            break;

        case ALONG_DATA:
            t->data = calloc(1, t->owner->data_size);
            if(!t->data)
            {
                err("Failed to allocate memory for trace data\n");
                return -ENOMEM;
            }

            memcpy(t->data, blk->cmp_data, t->owner->data_size);
            break;

        default:
            err("Unknown along enum value\n");
            return -EINVAL;
    }

    switch(cfg->stat)
    {
        case SUMMARY_AVG:
            ret = stat_get_mean_all(blk->acc, &t->samples);
            break;

        case SUMMARY_DEV:
            ret = stat_get_dev_all(blk->acc, &t->samples);
            break;

        case SUMMARY_MIN:
        case SUMMARY_MAX:
            err("Unimplemented\n");
            return -EINVAL;

        default:
            err("Unknown summary enum value\n");
            return -EINVAL;
    }

    if(ret < 0)
    {
        err("Failed to get summary statistic\n");
        return ret;
    }

    if(blk->cmp_data)
        free(blk->cmp_data);

    stat_free_accumulator(blk->acc);
    free(blk);
    return 0;
}

int tfm_reduce_along(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param)
{
    int ret;
    struct block_args block_args = {
            .consumer_init = tfm_reduce_along_init,
            .consumer_exit = tfm_reduce_along_exit,

            .initialize = tfm_reduce_along_initialize,
            .trace_interesting = tfm_reduce_along_interesting,
            .trace_matches = tfm_reduce_along_matches,
            .accumulate = tfm_reduce_along_accumulate,
            .finalize = tfm_reduce_along_finalize,

            .criteria = DONE_LISTLEN
    };

    struct tfm_reduce_along_config *cfg;
    cfg = calloc(1, sizeof(struct tfm_reduce_along_config));
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