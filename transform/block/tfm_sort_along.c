#include "trace.h"
#include "transform.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "statistics.h"
#include "list.h"

#include <string.h>
#include <errno.h>
#include <math.h>
#include <float.h>

struct tfm_sort_along_config
{
    summary_t stat;
    filter_t along;
    filter_param_t param;
};

struct tfm_sort_along_entry
{
    struct list_head list;
    struct trace *t;
    float val;
};

struct tfm_sort_along_block
{
    int count, finalized;
    void *cmp_data;

    struct list_head list;
    struct tfm_sort_along_entry *all_entries;
};

int tfm_sort_along_init(struct trace_set *ts, void *arg)
{
    struct tfm_sort_along_config *cfg = arg;
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

int tfm_sort_along_exit(struct trace_set *ts, void *arg)
{
    free(arg);
    return 0;
}

int tfm_sort_along_initialize(struct trace *t, void **block, void *arg)
{
    int ret;
    struct tfm_sort_along_block *new;
    struct tfm_sort_along_config *cfg = arg;

    new = calloc(1, sizeof(struct tfm_sort_along_block));
    if(!new)
    {
        err("Failed to allocate new block\n");
        return -ENOMEM;
    }

    LIST_HEAD_INIT_INLINE(new->list);
    new->finalized = 0;
    new->all_entries = NULL;

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

    *block = new;
    return 0;

__free_new:
    free(new);
    return ret;
}

bool tfm_sort_along_interesting(struct trace *t, void *arg)
{
    struct tfm_sort_along_config *cfg = arg;
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

bool tfm_sort_along_matches(struct trace *t, void *block, void *arg)
{
    struct tfm_sort_along_block *blk = block;
    struct tfm_sort_along_config *cfg = arg;

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

int tfm_sort_along_accumulate(struct trace *t, void *block, void *arg)
{
    int ret;
    float val;
    bool free_entry = false;

    struct tfm_sort_along_block *blk = block;
    struct tfm_sort_along_config *cfg = arg;
    struct tfm_sort_along_entry *entry;
    struct accumulator *acc;

    if(tfm_sort_along_matches(t, block, arg))
    {
        switch(cfg->along)
        {
            case ALONG_NUM:
            case ALONG_DATA:
                blk->count++;
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

    ret = stat_create_single(&acc, __summary_to_cability(cfg->stat));
    if(ret < 0)
    {
        err("Failed to create accumulator\n");
        return ret;
    }

    ret = stat_accumulate_single_many(acc, t->samples, t->owner->num_samples);
    if(ret < 0)
    {
        err("Failed to accumulate trace samples\n");
        goto __free_accumulator;
    }

    ret = stat_get(acc, __summary_to_cability(cfg->stat), 0, &val);
    if(ret < 0)
    {
        err("Failed to get value from accumulator\n");
        goto __free_accumulator;
    }

    entry = calloc(1, sizeof(struct tfm_sort_along_entry));
    if(!entry)
    {
        err("Failed to allocate new sorting entry\n");
        return -ENOMEM;
    }

    LIST_HEAD_INIT_INLINE(entry->list);
    entry->val = val;

    ret = trace_copy(&entry->t, t);
    if(ret < 0)
    {
        err("Failed to copy trace to entry\n");
        free_entry = true; goto __free_entry;
    }

    list_add_tail(&entry->list, &blk->list);
__free_entry:
    if(free_entry)
        free(entry);

__free_accumulator:
    stat_free_accumulator(acc);
    return ret;
}

int __compare_entries_gt(const void *e1, const void *e2)
{
    const struct tfm_sort_along_entry *entry1 = e1, *entry2 = e2;
    if(entry1->val < entry2->val)
        return 1;
    else if(entry1->val > entry2->val)
        return -1;
    else return 0;
}

int __compare_entries_lt(const void *e1, const void *e2)
{
    const struct tfm_sort_along_entry *entry1 = e1, *entry2 = e2;
    if(entry1->val > entry2->val)
        return 1;
    else if(entry1->val < entry2->val)
        return -1;
    else return 0;
}

int tfm_sort_along_finalize(struct trace *t, void *block, void *arg)
{
    int i, ret;
    struct tfm_sort_along_block *blk = block;
    struct tfm_sort_along_config *cfg = arg;
    struct tfm_sort_along_entry *pos, *n;

    if(!t)
    {
        err("Finalize called for invalid trace\n");
        return -EINVAL;
    }

    if(!blk->all_entries)
    {
        blk->all_entries = calloc(blk->count, sizeof(struct tfm_sort_along_entry));
        if(!blk->all_entries)
        {
            err("Failed to allocate flattened entry list\n");
            return -ENOMEM;
        }

        i = 0;
        list_for_each_entry_safe(pos, n, &blk->list, struct tfm_sort_along_entry, list)
        {
            memcpy(&blk->all_entries[i++], pos, sizeof(struct tfm_sort_along_entry));
            list_del(&pos->list);
            free(pos);
        }

        switch(cfg->stat)
        {
            case SUMMARY_MIN:
                qsort(blk->all_entries, blk->count,
                      sizeof(struct tfm_sort_along_entry),
                              __compare_entries_lt);
                break;

            case SUMMARY_MAX:
            case SUMMARY_MAXABS:
            case SUMMARY_MINABS:
                qsort(blk->all_entries, blk->count,
                      sizeof(struct tfm_sort_along_entry),
                              __compare_entries_gt);
                break;

            case SUMMARY_AVG:
            case SUMMARY_DEV:
                err("Invalid summary statistic for tfm_sort_along\n");
                return -EINVAL;
        }
    }

    pos = &blk->all_entries[blk->finalized];
    pos->t->owner = t->owner;

    ret = copy_title(t, pos->t);
    if(ret >= 0)
        ret = copy_data(t, pos->t);
    if(ret >= 0)
        ret = copy_samples(t, pos->t);
    if(ret < 0)
    {
        err("Failed to copy something\n");
        return ret;
    }

    pos->t->owner = NULL;
    trace_free_memory(pos->t);

    blk->finalized++;
    if(blk->finalized < blk->count)
        return 1;
    else
    {
        free(blk->all_entries);
        if(blk->cmp_data)
            free(blk->cmp_data);

        free(blk);
        return 0;
    }
}

int tfm_sort_along(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param)
{
    int ret;
    struct tfm_sort_along_config *cfg;
    struct block_args block_args = {
            .consumer_init = tfm_sort_along_init,
            .consumer_exit = tfm_sort_along_exit,
            .consumer_init_waiter = NULL,

            .initialize = tfm_sort_along_initialize,
            .trace_interesting = tfm_sort_along_interesting,
            .trace_matches = tfm_sort_along_matches,
            .accumulate = tfm_sort_along_accumulate,
            .finalize = tfm_sort_along_finalize,

            .criteria = DONE_LISTLEN
    };

    if(stat == SUMMARY_AVG || stat == SUMMARY_DEV)
    {
        err("Invalid summary statistic for sorting transformation\n");
        return -EINVAL;
    }

    cfg = calloc(1, sizeof(struct tfm_sort_along_config));
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