#include "extract_internal.h"

#include "__trace_internal.h"
#include "statistics.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>


int __find_pearson(struct trace *t, struct tfm_extract_config *cfg, float **res)
{
    return stat_pattern_match(cfg->ref.matcher,
                              t->samples, t->owner->num_samples, res);
}

void __find_local_max(struct tfm_extract_config *cfg,
                      struct tfm_extract_block *blk,
                      int base, float *max_val, int *max_i)
{
    int i, index = -1;
    float val = -FLT_MAX;

    for(i = -1 * cfg->max_dev; i < cfg->max_dev; i++)
    {
        if(base + i >= 0 && base + i < blk->num)
        {
            if(blk->pearson[base + i] > val)
            {
                val = blk->pearson[base + i];
                index = base + i;
            }
        }
    }

    *max_val = val;
    *max_i = index;
}

int __search_matches(struct tfm_extract_config *cfg,
                     struct tfm_extract_block *blk)
{
    int i, last = -1, timeout = 0;
    bool first = true;
    struct split_list_entry *new;

    blk->pearson[0] = fabsf(blk->pearson[0]);
    blk->pearson[blk->num - 1] = fabsf(blk->pearson[blk->num - 1]);

    blk->count_true = 0;
    for(i = 1; i < blk->num - 1; i++)
    {
        blk->pearson[i] = fabsf(blk->pearson[i]);

        // Sometimes, we'll have many closely clustered (only a few samples difference)
        // correlation local maxima which actually correspond to the same pattern match.
        // Therefore, set a timeout value when we find a true match which must expire
        // before we accept a new match.
        if(timeout > 0)
            timeout--;

        if(blk->pearson[i] >= cfg->pattern.confidence &&
           blk->pearson[i - 1] < blk->pearson[i] &&
           blk->pearson[i + 1] < blk->pearson[i] &&
           timeout == 0)
        {
            if(first)
            {
                blk->first_pattern_match = i;
                first = false;
            }

            debug("Found true match @ %i\n", i);

            blk->count_true++;
            new = calloc(1, sizeof(struct split_list_entry));
            if(!new)
            {
                err("Failed to allocate new split list entry\n");
                return -ENOMEM;
            }

            LIST_HEAD_INIT_INLINE(new->list);
            new->type = SPLIT_CONFIDENT;
            new->index = i;

            list_add_tail(&new->list, &blk->split_list);
            last = i;

            if(blk->matches)
                blk->matches[i] = 1.0f;
            timeout = cfg->max_dev;
        }
    }

    blk->last_pattern_match = last;
    return 0;
}

#define ZCONF_95        1.960f
#define ZCONF_99        2.576f
#define ZCONF_99_5      2.807f
#define ZCONF_99_9      3.291f

#define CONF            ZCONF_95

#define NORMALIZED(x, u, s)     ((x) - (u)) / (s)

int __search_gaps(struct tfm_extract_config *cfg,
                  struct tfm_extract_block *blk)
{
    int ret, i, last_index = -1;
    float gap, predicted_mean;
    bool predictable;

    int base, best_index;
    float best_value;

    struct accumulator *acc;
    struct split_list_entry *new, *pos;

    if(list_empty(&blk->split_list))
    {
        err("Cannot search for gaps with an empty split list\n");
        return -EINVAL;
    }

    ret = stat_create_single(&acc, STAT_AVG | STAT_DEV);
    if(ret < 0)
    {
        err("Failed to create accumulator for gap correlations\n");
        return ret;
    }

    blk->count_predictable = 0;
    blk->count_unpredictable = 0;

    list_for_each_entry(pos, &blk->split_list, struct split_list_entry, list)
    {
        if(pos->type == SPLIT_CONFIDENT)
        {
            if(last_index != -1)
            {
                if(pos->index - last_index < cfg->avg_len - cfg->max_dev ||
                   pos->index - last_index >= cfg->avg_len + cfg->max_dev)
                {
                    gap = ((float) (pos->index - last_index)) / cfg->ref.mean;
                    predicted_mean = ((float) (pos->index - last_index)) / roundf(gap);

                    predictable = (fabsf(NORMALIZED(predicted_mean,
                                                    cfg->ref.mean,
                                                    cfg->ref.dev)) <= CONF);

                    debug("Found %s gap (size %f) @ %i -> %i\n",
                          predictable ? "predictable" : "unpredictable",
                          gap, last_index, pos->index);

                    new = calloc(1, sizeof(struct split_list_entry));
                    if(!new)
                    {
                        err("Failed to allocate split list entry for detected gap\n");
                        ret = -ENOMEM;
                        goto __free_accumulator;
                    }

                    LIST_HEAD_INIT_INLINE(new->list);
                    LIST_HEAD_INIT_INLINE(new->unpredictable);

                    new->type = (predictable ?
                                 SPLIT_GAP_PREDICTABLE :
                                 SPLIT_GAP_UNPREDICTABLE);
                    new->index = pos->index;
                    new->num = (predictable ? (int) roundf(gap) - 1 : -1);
                    list_add_tail(&new->list, &pos->list);

                    if(predictable)
                    {
                        blk->count_predictable += new->num;
                        for(i = 0; i < new->num; i++)
                        {
                            base = last_index + (i + 1) * (int) cfg->ref.mean;
                            __find_local_max(cfg, blk, base,
                                             &best_value, &best_index);

                            ret = stat_accumulate_single(acc, best_value);
                            if(ret < 0)
                            {
                                err("Failed to accumulate into gap data accumulator\n");
                                goto __free_accumulator;
                            }
                        }

                        if(blk->pred)
                        {
                            for(i = last_index; i < pos->index; i++)
                                blk->pred[i] = 1.0f;
                        }
                    }
                    else
                    {
                        list_add_tail(&new->unpredictable, &blk->split_unpredictable);
                        blk->count_unpredictable++;

                        if(blk->unpred)
                        {
                            for(i = last_index; i < pos->index; i++)
                                blk->unpred[i] = 1.0f;
                        }
                    }
                }
            }

            last_index = pos->index;
        }
    }

    ret = stat_get(acc, STAT_AVG, 0, &blk->gap_mean);
    if(ret < 0)
    {
        err("Failed to get correlation mean\n");
        goto __free_accumulator;
    }

    ret = stat_get(acc, STAT_DEV, 0, &blk->gap_dev);
    if(ret < 0)
    {
        err("Failed to get correlation deviation\n");
        goto __free_accumulator;
    }

__free_accumulator:
    stat_free_accumulator(acc);
    return ret;
}


int __optimize_gaps(struct tfm_extract_config *cfg,
                    struct tfm_extract_block *blk)
{
    int i, ret;
    int base, front, back, num, last_num = -1;
    bool mismatch;
    struct split_list_entry *curr;

    int *indices_forwards, *indices_backwards;
    float *values_forwards, *values_backwards;
    indices_forwards = indices_backwards = NULL;
    values_forwards = values_backwards = NULL;

    blk->count_unpredictable = 0;
    list_for_each_entry(curr, &blk->split_unpredictable, struct split_list_entry, unpredictable)
    {
        if(curr->type != SPLIT_GAP_UNPREDICTABLE)
        {
            err("Non-unpredictable split list entry in unpredictable split list\n");
            ret = -EINVAL;
            goto __free_temp_arrays;
        }

        back = list_next_entry(curr, struct split_list_entry, list)->index;
        front = list_prev_entry(curr, struct split_list_entry, list)->index;
        num = (int) roundf((float) (back - front) / cfg->ref.mean) - 1;

        // reallocate temp buffers if needed
        if(last_num == -1 || num >= last_num)
        {
            if(indices_forwards) free(indices_forwards);
            if(indices_backwards) free(indices_backwards);
            if(values_forwards) free(values_forwards);
            if(values_backwards) free(values_backwards);

            indices_forwards = indices_backwards = NULL;
            values_forwards = values_backwards = NULL;

            // chain-allocate
            indices_forwards = calloc(num, sizeof(int));
            if(indices_forwards)
                indices_backwards = calloc(num, sizeof(int));
            if(indices_backwards)
                values_forwards = calloc(num, sizeof(float));
            if(values_forwards)
                values_backwards = calloc(num, sizeof(float));
            if(!values_backwards)
            {
                err("Failed to allocate a temp array of size %i\n", num);
                ret = -ENOMEM;
                goto __free_temp_arrays;
            }

            last_num = num;
        }

        // find maximums in both directions
        for(i = 0; i < num; i++)
        {
            base = front + (i + 1) * (int) cfg->ref.mean;
            __find_local_max(cfg, blk, base,
                             &values_forwards[i],
                             &indices_forwards[i]);

            base = back - (i + 1) * (int) cfg->ref.mean;
            __find_local_max(cfg, blk, base,
                             &values_backwards[num - i - 1],
                             &indices_backwards[num - i - 1]);
        }

        // if all entries agree, accept those
        mismatch = false;
        for(i = 0; i < num; i++)
        {
            if(indices_forwards[i] != indices_backwards[i])
            {
                debug("Forward and backward runs don't agree\n");
                mismatch = true;
                break;
            }

            if(values_forwards[i] < blk->gap_mean &&
               fabsf(NORMALIZED(values_forwards[i],
                                blk->gap_mean,
                                blk->gap_dev)) > CONF)
            {
                debug("Agreed intermediate not confidently a missed gap\n");
                mismatch = true;
                break;
            }
        }

        if(!mismatch)
        {
            curr->num = num;
            blk->count_unpredictable += num;
            continue;
        }

        // get the number of entries at the beginning and at the end
        for(i = 0; i < num; i++)
        {
            if(values_forwards[i] < blk->gap_mean &&
               fabsf(NORMALIZED(values_forwards[i],
                                blk->gap_mean,
                                blk->gap_dev)) > CONF)
            {
                // no longer a match
                break;
            }
        }

        curr->num = i;
        for(i = 0; i < num; i++)
        {
            if(values_backwards[num - i - 1] < blk->gap_mean &&
               fabsf(NORMALIZED(values_backwards[num - i - 1],
                                blk->gap_mean,
                                blk->gap_dev)) > CONF)
            {
                // no longer a match
                break;
            }
        }

        curr->num += i;
        blk->count_unpredictable += curr->num;
    }

    ret = 0;
__free_temp_arrays:
    if(indices_forwards)
        free(indices_forwards);
    if(indices_backwards)
        free(indices_backwards);
    if(values_forwards)
        free(values_forwards);
    if(values_backwards)
        free(values_backwards);

    return ret;
}

int __search_tail(struct tfm_extract_config *cfg,
                  struct tfm_extract_block *blk)
{
    int ret, i, base;
    int first = -1, last = -1;

    int num_means, max_index = 0;
    float *means, max_mean;

    float *max_values = NULL;
    int *max_indices = NULL;

    max_values = calloc(2 * blk->missing, sizeof(float));
    if(max_values)
        max_indices = calloc(2 * blk->missing, sizeof(int));

    if(!max_indices)
    {
        err("Failed to allocate some temp arrays\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    struct accumulator *acc;
    struct split_list_entry *new, *front;

    memset(max_values, 0, 2 * blk->missing * sizeof(float));
    memset(max_indices, 0xFF, 2 * blk->missing * sizeof(int));

    for(i = -1 * blk->missing; i < blk->missing; i++)
    {
        base = (i * (int) cfg->ref.mean) + (i < 0 ?
                                            blk->first_pattern_match :
                                            (blk->last_pattern_match +
                                             (int) cfg->ref.mean));

        __find_local_max(cfg, blk, base,
                         &max_values[blk->missing + i],
                         &max_indices[blk->missing + i]);

        // go ahead and invalidate ones which don't pass confidence testing
        // or are out of bounds
        if(fabsf(NORMALIZED(max_values[blk->missing + i],
                            blk->gap_mean,
                            blk->gap_dev)) <= CONF ||
           max_indices[blk->missing + i] + cfg->pattern_size >= blk->num)
        {
            max_indices[blk->missing + i] = -1;
            max_values[blk->missing + i] = 0.0f;
        }
    }

    for(i = 0; i < 2 * blk->missing; i++)
    {
        if(max_indices[i] != -1)
        {
            if(first == -1) first = i;
            last = i;
        }
    }

    // out of confidence-tested values, find the best ones
    if((last - first + 1) >= blk->missing)
    {
        num_means = last - first - blk->missing + 2;

        ret = stat_create_single_array(&acc, STAT_AVG, num_means);
        if(ret < 0)
        {
            err("Failed to create accumulator\n");
            return ret;
        }

        for(i = 0; i < blk->missing; i++)
        {
            ret = stat_accumulate_single_array(acc, &max_values[first + i], num_means);
            if(ret < 0)
            {
                err("Failed to accumulate");
                stat_free_accumulator(acc);
                return ret;
            }
        }

        ret = stat_get_all(acc, STAT_AVG, &means);
        if(ret < 0)
        {
            err("Failed to get means from accumulator\n");
            stat_free_accumulator(acc);
            return ret;
        }

        max_mean = -FLT_MAX;
        for(i = 0; i < num_means; i++)
        {
            if(means[i] > max_mean)
            {
                max_mean = means[i];
                max_index = i;
            }
        }

        free(means);
        stat_free_accumulator(acc);

        first += max_index;
        last = first + blk->missing - 1;
    }

    front = list_first_entry(&blk->split_list, struct split_list_entry, list);
    for(i = first; i <= last; i++)
    {
        new = calloc(1, sizeof(struct split_list_entry));
        if(!new)
        {
            err("Failed to allocate new split list entry\n");
            return -ENOMEM;
        }

        LIST_HEAD_INIT_INLINE(new->list);
        new->type = SPLIT_GAP_TAIL;
        new->index = max_indices[i];

        if(max_indices[i] < blk->first_pattern_match)
            list_add_tail(&new->list, &front->list);
        else
            list_add_tail(&new->list, &blk->split_list);

        if(blk->tail)
            blk->tail[max_indices[i]] = 1.0f;
    }

    blk->count_tail = (last - first + 1);

    ret = 0;
__free_temp:
    if(max_values)
        free(max_values);

    if(max_indices)
        free(max_indices);

    return ret;
}