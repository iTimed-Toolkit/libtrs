#include "transform.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "extract_internal.h"
#include "platform.h"

#define DEBUG_TITLE_SIZE        128

int tfm_extract_timing_init(struct trace_set *ts, void *arg)
{
    struct tfm_extract_config *cfg = arg;
    int normal_datasize;

    switch(cfg->data)
    {
        case AES128:
            normal_datasize = 48; // pt, ct, key
            break;

        default:
        err("Unknown data format type\n");
            return -EINVAL;
    }

    if(ts->prev->data_size != normal_datasize + cfg->expecting * sizeof(uint64_t))
    {
        err("Invalid data size in previous data set -- no timing data?\n");
        return -EINVAL;
    }

    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;

    ts->data_size = ts->prev->data_size - cfg->expecting * sizeof(uint64_t);
    ts->num_samples = cfg->pattern_size;
    return 0;
}

int tfm_extract_timing_exit(struct trace_set *ts, void *arg)
{
    err("unimplemented\n");
}

int tfm_extract_timing_init_waiter(struct trace_set *ts, port_t port, void *arg)
{
    struct tfm_extract_config *cfg = arg;
    if(port == PORT_EXTRACT_TIMING_DEBUG)
    {
        ts->title_size = DEBUG_TITLE_SIZE;
        ts->data_size = 0;
        ts->datatype = DT_BYTE;
        ts->yscale = 1.0f / 127.0f;

        ts->num_traces = 3 * ts_num_traces(ts->prev->prev);
        ts->num_samples = ts_num_samples(ts->prev->prev) - NUM_MATCH(&cfg->pattern);

        cfg->debugging = true;
        return 0;
    }
    else
    {
        err("Invalid port for pattern extraction\n");
        return -EINVAL;
    }
}

int tfm_extract_timing_initialize(struct trace *t, void **block, void *arg)
{
    int ret;
    struct tfm_extract_config *cfg = arg;
    struct tfm_extract_block *new;

    new = calloc(1, sizeof(struct tfm_extract_block));
    if(!new)
    {
        err("Failed to allocate new block\n");
        return -ENOMEM;
    }

    LIST_HEAD_INIT_INLINE(new->split_list);
    LIST_HEAD_INIT_INLINE(new->split_unpredictable);

    new->index = TRACE_IDX(t);
    new->num = (int) t->owner->num_samples - NUM_MATCH(&cfg->pattern);
    new->trace.owner = t->owner;

    switch(cfg->data)
    {
        case AES128:
            new->trace.data = calloc(48, sizeof(uint8_t));
            if(!new->trace.data)
            {
                err("Failed to allocate new data array\n");
                ret = -ENOMEM; goto __free_new;
            }

            memcpy(new->trace.data, t->data, 32);
            memcpy(&new->trace.data[32], &t->data[32 + 8 * cfg->expecting], 16);
            break;

        default:
            err("Unknown data format type\n");
            return -EINVAL;
    }

    ret = copy_samples(&new->trace, t);
    if(ret < 0)
    {
        err("Failed to copy samples\n");
        goto __free_new;
    }

    *block = new;
    return 0;

__free_new:
    if(new->trace.data)
        free(new->trace.data);

    if(new->trace.samples)
        free(new->trace.samples);

    free(new);
    return ret;
}

bool tfm_extract_timing_interesting(struct trace *t, void *arg)
{
    return (t->samples && t->data);
}

bool tfm_extract_timing_matches(struct trace *t, void *block, void *arg)
{
    // each trace in its own block -- also enforced by DONE_SINGULAR
    return false;
}

extern int __process_ref_trace(struct trace *t, struct tfm_extract_config *cfg);

int tfm_extract_timing_accumulate(struct trace *t, void *block, void *arg)
{
    int ret, index, i;
    struct tfm_extract_config *cfg = arg;
    struct tfm_extract_block *blk = block;

    sem_acquire(&cfg->lock);
    if(!cfg->ref_valid)
    {
        ret = __process_ref_trace(t, cfg);
        if(ret < 0)
        {
            err("Failed to process reference trace\n");
            return ret;
        }

        cfg->ref_valid = true;
    }
    sem_release(&cfg->lock);

    uint64_t *timings;
    switch(cfg->data)
    {
        case AES128:
            timings = (uint64_t *) &t->data[32];
            break;

        default:
        err("Unknown data format\n");
            return -EINVAL;
    }

    ret = __find_pearson(t, cfg, &blk->pearson);
    if(ret < 0)
    {
        err("Failed to find Pearson correlation\n");
        return ret;
    }

    if(cfg->debugging)
    {
        blk->matches = calloc(blk->num, sizeof(float));
        if(!blk->matches)
        {
            err("Failed to allocate debug match array\n");
            ret = -ENOMEM;
            goto __out;
        }

        blk->timing = calloc(blk->num, sizeof(float));
        if(!blk->timing)
        {
            err("Failed to allocate debug timing array\n");
            free(blk->matches);
            ret = -ENOMEM;
            goto __out;
        }

        for(i = 0; i < cfg->expecting; i++)
        {
            index = (int) ((float) (timings[i] - timings[0]) / (t->owner->xscale * 1e9f)) + 5227;
            blk->timing[index] = 1.0f;
        }
    }

    ret = __search_matches(cfg, blk);
    if(ret < 0)
    {
        err("Failed to search for matches\n");
        goto __out;
    }

    int timing_index = 0, last_gap = -1, num_gap, found = 0;
    struct split_list_entry *curr = list_first_entry(&blk->split_list, struct split_list_entry, list),
            *first_match = curr, *last_match = list_last_entry(&blk->split_list, struct split_list_entry, list),
            *new;

    while(timing_index < cfg->expecting &&
          curr != list_last_entry(&blk->split_list, struct split_list_entry, list))
    {
        index = (int) ((float) (timings[timing_index] - timings[0]) / (t->owner->xscale * 1e9f)) + 5227;

        // todo this needs tuning, and probably a separate optimization step to find the "best" offset ^
        if(abs(index - curr->index) < cfg->max_dev)
        {
            // these are close, so we keep these as a match and advance
            if(last_gap != -1)
            {
                new = calloc(1, sizeof(struct split_list_entry));
                if(!new)
                {
                    err("Failed to allocate new split list entry\n");
                    ret = -ENOMEM;
                    goto __out;
                }

                LIST_HEAD_INIT_INLINE(new->list);
                new->index = last_gap;
                new->num = num_gap;
                new->type = SPLIT_GAP_PREDICTABLE;
                list_add_tail(&new->list, &curr->list);

                last_gap = -1;
            }

            found++;
            timing_index++;
            curr = list_next_entry(curr, struct split_list_entry, list);
            continue;
        }
        else
        {
            if(curr == first_match)
            {
                new = calloc(1, sizeof(struct split_list_entry));
                if(!new)
                {
                    err("Failed to allocate new split list entry\n");
                    ret = -ENOMEM;
                    goto __out;
                }

                LIST_HEAD_INIT_INLINE(new->list);
                new->index = index;
                new->num = 1;
                new->type = SPLIT_GAP_TAIL;
                list_add_tail(&new->list, &curr->list);
                found++;
            }
            else if(curr == last_match)
            {
                new = calloc(1, sizeof(struct split_list_entry));
                if(!new)
                {
                    err("Failed to allocate new split list entry\n");
                    ret = -ENOMEM;
                    goto __out;
                }

                LIST_HEAD_INIT_INLINE(new->list);
                new->index = index;
                new->num = 1;
                new->type = SPLIT_GAP_TAIL;
                list_add(&new->list, &curr->list);
                found++;
            }
            else
            {
                // these are gaps
                if(last_gap == -1)
                {
                    last_gap = index;
                    num_gap = 1;
                }
                else num_gap++;
            }

            timing_index++;
        }
    }

    warn("Successfully extracted %i timing patterns for trace %li\n",
         found, TRACE_IDX(t));
__out:
    if(ret < 0 || !cfg->debugging)
        free(blk->pearson);

    return ret;
}

extern int __increment_data(struct tfm_extract_config *cfg, struct tfm_extract_block *blk);
extern int __finalize_data(struct tfm_extract_config *cfg, struct tfm_extract_block *blk, uint8_t *data);

int tfm_extract_timing_finalize(struct trace *t, void *block, void *arg)
{
    int i, ret;
    bool cont;

    struct tfm_extract_block *blk = block;
    struct tfm_extract_config *cfg = arg;
    struct split_list_entry *entry, *next;

    char title[DEBUG_TITLE_SIZE];
    if(cfg->debugging && !blk->debug_sent)
    {
        // Pearson correlation
        memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
        snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Pearson", blk->index);
        ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_TIMING_DEBUG, 4,
                                 3 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 0,
                                 title, NULL, blk->pearson);
        if(ret < 0)
        {
            err("Failed to push pearson to consumer\n");
            return ret;
        }

        // True matches
        memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
        snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Matches", blk->index);
        ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_TIMING_DEBUG, 4,
                                 3 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 1,
                                 title, NULL, blk->matches);
        if(ret < 0)
        {
            err("Failed to push true matches to consumer\n");
            return ret;
        }

        // Timing values
        memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
        snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Timings", blk->index);
        ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_TIMING_DEBUG, 4,
                                 3 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 2,
                                 title, NULL, blk->timing);
        if(ret < 0)
        {
            err("Failed to push timing values to consumer\n");
            return ret;
        }

        free(blk->pearson);
        free(blk->matches);
        free(blk->timing);
        blk->debug_sent = 1;
    }

    if(list_empty(&blk->split_list))
    {
        warn("Finalize called with empty split list\n");
        t->title = NULL;
        t->data = NULL;
        t->samples = NULL;
        goto __done;
    }

    entry = list_first_entry(&blk->split_list, struct split_list_entry, list);
    list_del(&entry->list);

    if(!(entry->type == SPLIT_CONFIDENT ||
         entry->type == SPLIT_GAP_TAIL))
    {
        err("List was left in incorrect position\n");
        return -EINVAL;
    }

    if(entry->index + cfg->pattern_size >= blk->num)
    {
        err("Entry found with not enough samples afterwards!\n");
        return -EINVAL;
    }

    // let tfm_block fill title
    t->title = NULL;

    // copy data
    t->data = calloc(t->owner->data_size, sizeof(uint8_t));
    if(!t->data)
    {
        err("Failed to allocate data array\n");
        return -ENOMEM;
    }

    memcpy(t->data, blk->trace.data, t->owner->data_size);

    // copy samples
    t->samples = calloc(cfg->pattern_size, sizeof(float));
    if(!t->samples)
    {
        err("Failed to allocate array for samples\n");
        ret = -ENOMEM;
        goto __free_data;
    }

    memcpy(t->samples, &blk->trace.samples[entry->index], cfg->pattern_size * sizeof(float));

    // increment data
    ret = __increment_data(cfg, blk);
    if(ret < 0)
    {
        err("Failed to increment data\n");
        goto __free_samples;
    }

    ret = __finalize_data(cfg, blk, t->data);
    if(ret < 0)
    {
        err("Failed to finalize data\n");
        goto __free_samples;
    }

    // fast-forward list
    cont = true;
    while(!list_empty(&blk->split_list) && cont)
    {
        next = list_first_entry(&blk->split_list, struct split_list_entry, list);
        switch(next->type)
        {
            case SPLIT_GAP_PREDICTABLE:
            case SPLIT_GAP_UNPREDICTABLE:
                for(i = 0; i < next->num; i++)
                {
                    ret = __increment_data(cfg, blk);
                    if(ret < 0)
                    {
                        err("Failed to increment data\n");
                        goto __free_samples;
                    }
                }

                list_del(&next->list);
                free(next);
                break;

            case SPLIT_CONFIDENT:
            case SPLIT_GAP_TAIL:
                cont = false;
                break;

            default:
            err("Unknown split type in entry\n");
                goto __free_samples;
        }
    }

__done:
    if(list_empty(&blk->split_list))
    {
        free(blk->trace.data);
        free(blk->trace.samples);
        free(blk);
        return 0;
    }
    else return 1;

__free_samples:
    free(t->samples);

__free_data:
    free(t->data);
    return ret;
}

int tfm_extract_timing(struct tfm **tfm, int pattern_size, int expecting, int avg_len, int max_dev,
                       match_region_t *pattern, crypto_t data)
{
    int ret;
    struct tfm_extract_config *cfg;
    struct block_args block_args = {
            .consumer_init = tfm_extract_timing_init,
            .consumer_exit = tfm_extract_timing_exit,
            .consumer_init_waiter = tfm_extract_timing_init_waiter,

            .initialize = tfm_extract_timing_initialize,
            .trace_interesting = tfm_extract_timing_interesting,
            .trace_matches = tfm_extract_timing_matches,
            .accumulate = tfm_extract_timing_accumulate,
            .finalize = tfm_extract_timing_finalize,

            .criteria = DONE_SINGULAR
    };

    cfg = calloc(1, sizeof(struct tfm_extract_config));
    if(!cfg)
    {
        err("Failed to allocate config struct\n");
        return -ENOMEM;
    }

    ret = p_sem_create(&cfg->lock, 1);
    if(ret < 0)
    {
        err("Failed to create config lock\n");
        free(cfg);
        return -errno;
    }

    memcpy(&cfg->pattern, pattern, sizeof(match_region_t));

    cfg->pattern_size = pattern_size;
    cfg->expecting = expecting;
    cfg->avg_len = avg_len;
    cfg->max_dev = max_dev;
    cfg->data = data;
    cfg->ref_valid = false;
    cfg->debugging = false;
    block_args.arg = cfg;

    ret = tfm_block(tfm, &block_args);
    if(ret < 0)
    {
        err("Failed to initialize generic block transform\n");
        p_sem_destroy(&cfg->lock);
        free(cfg);
        return ret;
    }

    return 0;
}