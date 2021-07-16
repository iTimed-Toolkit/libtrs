#include "transform.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "extract_internal.h"
#include "crypto.h"
#include "platform.h"

#include <string.h>
#include <errno.h>

#define DEBUG_TITLE_SIZE        128

int tfm_extract_pattern_init(struct trace_set *ts, void *arg)
{
    struct tfm_extract_config *cfg = arg;

    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;

    ts->data_size = ts->prev->data_size;
    ts->num_samples = cfg->pattern_size;
    return 0;
}

int tfm_extract_pattern_exit(struct trace_set *ts, void *arg)
{
    struct tfm_extract_config *cfg = arg;

    sem_acquire(&cfg->lock);
    p_sem_destroy(&cfg->lock);

#if USE_GPU
    gpu_pattern_free(cfg->ref.match_pattern);
#else
    free(cfg->ref.match_pattern);
#endif
    free(cfg);
    return 0;
}

int tfm_extract_pattern_init_waiter(struct trace_set *ts, port_t port, void *arg)
{
    struct tfm_extract_config *cfg = arg;
    if(port == PORT_EXTRACT_PATTERN_DEBUG)
    {
        ts->title_size = DEBUG_TITLE_SIZE;
        ts->data_size = 0;
        ts->datatype = DT_BYTE;
        ts->yscale = 1.0f / 127.0f;

        ts->num_traces = 5 * ts_num_traces(ts->prev->prev);
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

int __process_ref_trace(struct trace *t, struct tfm_extract_config *cfg)
{
    int ret, last_index;
    struct trace *ref_trace;
    struct split_list_entry *pos, *n;

    struct accumulator *acc;
    struct tfm_extract_block blk = {
            .split_list = LIST_HEAD_INIT(blk.split_list),
            .split_unpredictable = LIST_HEAD_INIT(blk.split_unpredictable),
            .count_true = 0, .count_predictable = 0,
            .count_unpredictable = 0, .missing = 0,
            .first_pattern_match = -1, .last_pattern_match = -1,
            .num = -1, .pearson = NULL
    };

    blk.num = (int) t->owner->num_samples - NUM_MATCH(&cfg->pattern);

    ret = trace_get(t->owner, &ref_trace, cfg->pattern.ref_trace);
    if(ret < 0)
    {
        err("Failed to get reference trace\n");
        return ret;
    }

    if(ref_trace->samples && ref_trace->data)
    {
#if USE_GPU
        ret = gpu_pattern_preprocess(&ref_trace->samples[cfg->pattern.lower],
                                     NUM_MATCH(&cfg->pattern),
                                     &cfg->ref.match_pattern,
                                     &cfg->ref.s_pattern);
        if(ret < 0)
        {
            err("Failed to preprocess pattern for GPU\n");
            goto __fail_free_trace;
        }

#else
        cfg->ref.match_pattern = calloc(NUM_MATCH(&cfg->pattern), sizeof(float));
        if(!cfg->ref.match_pattern)
        {
            err("Failed to allocate memory for reference pattern\n");
            ret = -ENOMEM;
            goto __fail_free_patterns;
        }

        memcpy(cfg->ref.match_pattern,
               &ref_trace->samples[cfg->pattern.lower],
               NUM_MATCH(&cfg->pattern) * sizeof(float));
#endif
    }
    else
    {
        err("Reference trace has no samples or data\n");
        ret = -EINVAL;
        goto __fail_free_trace;
    }

    ret = stat_create_single(&acc);
    if(ret < 0)
    {
        err("Failed to create accumulator for gap analysis\n");
        goto __fail_free_patterns;
    }

    ret = __find_pearson(ref_trace, cfg, &blk.pearson);
    if(ret < 0)
    {
        err("Failed to find pearson for reference trace\n");
        goto __fail_free_gap_acc;
    }

    trace_free(ref_trace);
    ref_trace = NULL;

    // find the true matches
    ret = __search_matches(cfg, &blk);
    if(ret < 0)
    {
        err("Failed to search for matches\n");
        goto __fail_free_pearson;
    }

    last_index = -1;
    cfg->ref.count = 0;
    list_for_each_entry_safe(pos, n, &blk.split_list, struct split_list_entry, list)
    {
        if(pos->type == SPLIT_CONFIDENT)
        {
            if(last_index != -1)
            {
                if(pos->index - last_index >= cfg->avg_len - cfg->max_dev &&
                   pos->index - last_index < cfg->avg_len + cfg->max_dev)
                {
                    ret = stat_accumulate_single(acc, (float) (pos->index - last_index));
                    if(ret < 0)
                    {
                        err("Failed to accumulate gap\n");
                        goto __fail_free_pearson;
                    }

                    cfg->ref.count += 1.0f;
                }
            }

            last_index = pos->index;
        }

        list_del(&pos->list);
        free(pos);
    }

    ret = stat_get_mean(acc, 0, &cfg->ref.mean);
    if(ret < 0)
    {
        err("Failed to get mean from accumulator\n");
        goto __fail_free_pearson;
    }

    ret = stat_get_dev(acc, 0, &cfg->ref.dev);
    if(ret < 0)
    {
        err("Failed to get dev from accumulator\n");
        goto __fail_free_pearson;
    }

    stat_free_accumulator(acc);
    free(blk.pearson);
    return 0;

__fail_free_pearson:
    free(blk.pearson);

__fail_free_gap_acc:
    stat_free_accumulator(acc);

__fail_free_patterns:
    if(cfg->ref.match_pattern)
    {
#if USE_GPU
        gpu_pattern_free(cfg->ref.match_pattern);
#else
        free(cfg->ref.match_pattern);
#endif
    }

__fail_free_trace:
    if(ref_trace)
        trace_free(ref_trace);

    return ret;
}

int tfm_extract_pattern_initialize(struct trace *t, void **block, void *arg)
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

    ret = copy_data(&new->trace, t);
    if(ret >= 0)
        ret = copy_samples(&new->trace, t);

    if(ret < 0)
    {
        err("Failed to copy something\n");
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

bool tfm_extract_pattern_interesting(struct trace *t, void *arg)
{
    return (t->samples && t->data);
}

bool tfm_extract_pattern_matches(struct trace *t, void *block, void *arg)
{
    // each trace in its own block -- also enforced by DONE_SINGULAR
    return false;
}

int __allocate_debug_arrays(struct tfm_extract_block *blk)
{
    blk->matches = blk->pred =
    blk->unpred = blk->tail = NULL;

    // chain allocate
    blk->matches = calloc(blk->num, sizeof(float));
    if(blk->matches)
        blk->pred = calloc(blk->num, sizeof(float));
    if(blk->pred)
        blk->unpred = calloc(blk->num, sizeof(float));
    if(blk->unpred)
        blk->tail = calloc(blk->num, sizeof(float));

    if(!blk->tail)
    {
        err("Failed to allocate some debug array\n");

        if(blk->matches) free(blk->matches);
        if(blk->pred) free(blk->pred);
        if(blk->unpred) free(blk->unpred);
        if(blk->tail) free(blk->tail);

        return -ENOMEM;
    }

    return 0;
}

int __send_debug(struct trace *t, struct tfm_extract_block *blk)
{
    int ret;
    char title[DEBUG_TITLE_SIZE];

    // Pearson correlation
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Pearson", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 0,
                             title, NULL, blk->pearson);
    if(ret < 0)
    {
        err("Failed to push pearson to consumer\n");
        return ret;
    }

    // True matches
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Matches", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 1,
                             title, NULL, blk->matches);
    if(ret < 0)
    {
        err("Failed to push true matches to consumer\n");
        return ret;
    }

    // Predictable gaps
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Predictable Gaps", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 2,
                             title, NULL, blk->pred);
    if(ret < 0)
    {
        err("Failed to push predictable gaps to consumer\n");
        return ret;
    }

    // Unpredictable gaps
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Unpredictable Gaps", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 3,
                             title, NULL, blk->unpred);
    if(ret < 0)
    {
        err("Failed to push unpredictable gaps to consumer\n");
        return ret;
    }

    // Tail values
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Tail", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index - (BLK_DEBUG ? BLK_DEBUG_FIRST : 0)) + 4,
                             title, NULL, blk->tail);
    if(ret < 0)
    {
        err("Failed to push tail values to consumer\n");
        return ret;
    }

    return 0;
}

int tfm_extract_pattern_accumulate(struct trace *t, void *block, void *arg)
{
    int ret;
    struct tfm_extract_config *cfg = arg;
    struct tfm_extract_block *blk = block;
    struct split_list_entry *curr, *n;

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

    // find pearson correlation
    ret = __find_pearson(t, cfg, &blk->pearson);
    if(ret < 0)
    {
        err("Failed to find Pearson correlation\n");
        return ret;
    }

    if(cfg->debugging)
    {
        ret = __allocate_debug_arrays(blk);
        if(ret < 0)
        {
            err("Failed to allocate debug arrays\n");
            goto __done;
        }
    }

    // use pearson correlation to find true matches
    ret = __search_matches(cfg, blk);
    if(ret < 0)
    {
        err("Failed to search for matches\n");
        goto __free_debug;
    }

    debug("Trace %zu: found %i true matches\n", TRACE_IDX(t), blk->count_true);

    if(blk->count_true == 0)
    {
        warn("No true matches in trace %zu, discarding\n", TRACE_IDX(t));
        ret = 0;
        goto __free_debug;
    }

    // find gaps
    ret = __search_gaps(cfg, blk);
    if(ret < 0)
    {
        err("Failed to search for gaps\n");
        goto __free_debug;
    }

    debug("Trace %zu: found %i predictable gaps, %i unpredictable gaps\n",
          TRACE_IDX(t), blk->count_predictable, blk->count_unpredictable);

    if(blk->count_unpredictable > 0)
    {
        debug("Found unpredictable gaps in trace\n");
        ret = __optimize_gaps(cfg, blk);
        if(ret < 0)
        {
            err("Failed to optimize unpredictable gaps\n");
            goto __free_debug;
        }
    }

    blk->missing = cfg->expecting - blk->count_true -
                   blk->count_predictable - blk->count_unpredictable;
    if(blk->missing > 0)
    {
        ret = __search_tail(cfg, blk);
        if(ret < 0)
        {
            err("Failed to find tail matches\n");
            goto __free_debug;
        }
    }

    warn("Trace %zu: C = %i, P = %i, U = %i, T = %i, total %i\n",
         TRACE_IDX(t), blk->count_true, blk->count_predictable, blk->count_unpredictable, blk->count_tail,
         blk->count_true + blk->count_predictable + blk->count_unpredictable + blk->count_tail);

    if(blk->count_true + blk->count_predictable +
       blk->count_unpredictable + blk->count_tail != cfg->expecting)
    {
        debug("Rejecting trace\n");
        list_for_each_entry_safe(curr, n, &blk->split_list, struct split_list_entry, list)
        {
            list_del(&curr->list);
            free(curr);
        }
    }

    ret = 0;
__free_debug:
    if(ret < 0 && cfg->debugging)
    {
        free(blk->matches);
        free(blk->pred);
        free(blk->unpred);
        free(blk->tail);
    }

__done:
    if(ret < 0 || !cfg->debugging)
        free(blk->pearson);

    return ret;
}

int __increment_data(struct tfm_extract_config *cfg,
                     struct tfm_extract_block *blk)
{
    switch(cfg->data)
    {
        case AES128:
            // increment plaintext
            return encrypt_aes128(&blk->trace.data[0],
                                  &blk->trace.data[32],
                                  &blk->trace.data[0]);

        default:
            err("Unknown data increment function\n");
            return -EINVAL;
    }
}

int __finalize_data(struct tfm_extract_config *cfg,
                    struct tfm_extract_block *blk,
                    uint8_t *data)
{
    switch(cfg->data)
    {
        case AES128:
            // place ciphertext correctly
            return encrypt_aes128(&data[0],
                                  &blk->trace.data[32],
                                  &data[16]);

        default:
            err("Unknown data increment function\n");
            return -EINVAL;
    }
}

int tfm_extract_pattern_finalize(struct trace *t, void *block, void *arg)
{
    int i, ret;
    bool cont;

    struct tfm_extract_block *blk = block;
    struct tfm_extract_config *cfg = arg;
    struct split_list_entry *entry, *next;

    if(cfg->debugging && !blk->debug_sent)
    {
        ret = __send_debug(t, blk);
        if(ret < 0)
        {
            err("Failed to send debug to next tfm\n");
            return ret;
        }

        free(blk->pearson);
        free(blk->matches);
        free(blk->pred);
        free(blk->unpred);
        free(blk->tail);
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

int tfm_extract_pattern(struct tfm **tfm, int pattern_size, int expecting, int avg_len, int max_dev,
                        match_region_t *pattern, crypto_t data)
{
    int ret;
    struct tfm_extract_config *cfg;
    struct block_args block_args = {
            .consumer_init = tfm_extract_pattern_init,
            .consumer_exit = tfm_extract_pattern_exit,
            .consumer_init_waiter = tfm_extract_pattern_init_waiter,

            .initialize = tfm_extract_pattern_initialize,
            .trace_interesting = tfm_extract_pattern_interesting,
            .trace_matches = tfm_extract_pattern_matches,
            .accumulate = tfm_extract_pattern_accumulate,
            .finalize = tfm_extract_pattern_finalize,

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