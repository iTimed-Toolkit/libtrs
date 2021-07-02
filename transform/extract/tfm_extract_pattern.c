#include "transform.h"
#include "statistics.h"

#include "__tfm_internal.h"

#include "trace.h"
#include "__trace_internal.h"

#include "crypto.h"
#include "list.h"

#include <string.h>
#include <errno.h>
#include <math.h>
#include <float.h>
#include <unistd.h>

#define NUM_MATCH(match)        ((match)->upper - (match)->lower)
#define DEBUG_TITLE_SIZE        128

#define USE_GPU                 0
#define USE_NET                 1

#if USE_NET

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define NETADDR                 "achilles.home"
#define NETPORT                 9936

#endif

#if USE_GPU && USE_NET
#error "Invalid configuration"
#endif

struct tfm_extract_reference
{
    float mean, dev, count;
    float *match_pattern, s_pattern;
};

struct split_list_entry
{
    struct list_head list;
    struct list_head unpredictable;

    enum split_type
    {
        SPLIT_CONFIDENT,
        SPLIT_GAP_PREDICTABLE,
        SPLIT_GAP_UNPREDICTABLE,
        SPLIT_GAP_TAIL
    } type;

    int index, num;
};

struct tfm_extract_pattern_config
{
    // configuration
    sem_t lock;
    match_region_t pattern;
    int pattern_size, expecting, avg_len, max_dev;
    crypto_t data;

    // reference trace
    bool ref_valid;
    struct tfm_extract_reference ref;

#if USE_NET
    struct hostent *server;
    struct sockaddr_in addr;
#endif

    // debug
    bool debugging;
};

struct tfm_extract_pattern_block
{
    struct list_head split_list;
    struct list_head split_unpredictable;

    // counts for the four match groups
    int count_true, count_predictable,
            count_unpredictable, count_tail,
            missing;
    float gap_mean, gap_dev;

    // first/last found pattern match
    int first_pattern_match, last_pattern_match;

    // various arrays
    int index, num, debug_sent;
    float *pearson, *matches, *pred, *unpred, *tail;

    // intermediate data derivation
    struct trace trace;
};

int tfm_extract_pattern_init(struct trace_set *ts, void *arg)
{
    struct tfm_extract_pattern_config *cfg = arg;

    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;

    ts->data_size = ts->prev->data_size;
    ts->num_samples = cfg->pattern_size;
    return 0;
}

int tfm_extract_pattern_exit(struct trace_set *ts, void *arg)
{
    struct tfm_extract_pattern_config *cfg = arg;

    sem_acquire(&cfg->lock);
    sem_destroy(&cfg->lock);

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
    struct tfm_extract_pattern_config *cfg = arg;

    if(port == PORT_EXTRACT_PATTERN_DEBUG)
    {
        ts->title_size = DEBUG_TITLE_SIZE;
        ts->data_size = 0;
        ts->datatype = DT_BYTE;
        ts->yscale = 1.0f / 255.0f;

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

int __search_matches(struct tfm_extract_pattern_config *cfg,
                     struct tfm_extract_pattern_block *blk)
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

void __find_local_max(struct tfm_extract_pattern_config *cfg,
                      struct tfm_extract_pattern_block *blk,
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

#define ZCONF_95        1.960f
#define ZCONF_99        2.576f
#define ZCONF_99_5      2.807f
#define ZCONF_99_9      3.291f

#define CONF            ZCONF_95

#define NORMALIZED(x, u, s)     ((x) - (u)) / (s)

int __search_gaps(struct tfm_extract_pattern_config *cfg,
                  struct tfm_extract_pattern_block *blk)
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

    ret = stat_create_single(&acc);
    if(ret < 0)
    {
        err("Failed to create accumulator for gap correlations\n");
        return ret;
    }

    blk->count_predictable = 0;
    blk->count_unpredictable = 0;

    list_for_each_entry(pos, &blk->split_list, list)
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

    ret = stat_get_mean(acc, 0, &blk->gap_mean);
    if(ret < 0)
    {
        err("Failed to get correlation mean\n");
        goto __free_accumulator;
    }

    ret = stat_get_dev(acc, 0, &blk->gap_dev);
    if(ret < 0)
    {
        err("Failed to get correlation deviation\n");
        goto __free_accumulator;
    }

__free_accumulator:
    stat_free_accumulator(acc);
    return ret;
}

int __optimize_gaps(struct tfm_extract_pattern_config *cfg,
                    struct tfm_extract_pattern_block *blk)
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
    list_for_each_entry(curr, &blk->split_unpredictable, unpredictable)
    {
        if(curr->type != SPLIT_GAP_UNPREDICTABLE)
        {
            err("Non-unpredictable split list entry in unpredictable split list\n");
            ret = -EINVAL;
            goto __free_temp_arrays;
        }

        back = list_next_entry(curr, list)->index;
        front = list_prev_entry(curr, list)->index;
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

int __search_tail(struct tfm_extract_pattern_config *cfg,
                  struct tfm_extract_pattern_block *blk)
{
    int ret, i, base;
    int first = -1, last = -1;

    int num_means, max_index;
    float *means, max_mean;

    float max_values[2 * blk->missing];
    int max_indices[2 * blk->missing];

    struct accumulator *acc;
    struct split_list_entry *new, *front;

    memset(max_values, 0, sizeof(max_values));
    memset(max_indices, 0xFF, sizeof(max_indices));

    for(i = -1 * blk->missing; i < blk->missing; i++)
    {
        base = (i * (int) cfg->ref.mean) + (i < 0 ?
                                            blk->first_pattern_match :
                                            (blk->last_pattern_match +
                                             (int) cfg->ref.mean));

        __find_local_max(cfg, blk, base,
                         &max_values[blk->missing + i],
                         &max_indices[blk->missing + i]);
    }

    for(i = 0; i < 2 * blk->missing; i++)
    {
        if(max_indices[i] != -1)
        {
            if(first == -1) first = i;
            last = i;
        }
    }

    if((last - first + 1) >= blk->missing)
    {
        num_means = last - first - blk->missing + 2;

        ret = stat_create_single_array(&acc, num_means);
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

        ret = stat_get_mean_all(acc, &means);
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
    return 0;
}

int __find_pearson(struct trace *t, struct tfm_extract_pattern_config *cfg, float **res)
{
#if USE_GPU
    int i, ret;
    ret = gpu_pattern_match(t->samples, t->owner->num_samples,
                            cfg->ref.match_pattern,
                            NUM_MATCH(&cfg->pattern),
                            cfg->ref.s_pattern, res);

#elif USE_NET
    int i, ret, sockfd;
    bool success;

    int data_len = (int) t->owner->num_samples;
    int pattern_len = (int) NUM_MATCH(&cfg->pattern);
    int res_len = (data_len - pattern_len);
    size_t written;

    float *result = calloc(res_len, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result array\n");
    }

    if(!cfg->ref_valid)
    {
        cfg->server = gethostbyname(NETADDR);
        if(!cfg->server)
        {
            err("Failed to look up server name\n");
            return -h_errno;
        }

        cfg->addr.sin_family = AF_INET;
        cfg->addr.sin_port = htons(NETPORT);
        memcpy(&cfg->addr.sin_addr.s_addr,
               cfg->server->h_addr, cfg->server->h_length);
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0)
    {
        err("Failed to create socket: %s\n", strerror(errno));
        return -errno;
    }

    ret = connect(sockfd, (struct sockaddr *) &cfg->addr, sizeof(cfg->addr));
    if(ret < 0)
    {
        err("Failed to connect socket: %s\n", strerror(errno));
        ret = -errno;
        goto __close_socket;
    }

    FILE *netfile = fdopen(sockfd, "rw+");
    if(!netfile)
    {
        err("Failed to open a FILE* from sockfd: %s\n", strerror(errno));
        ret = -errno;
        goto __close_socket;
    }

    success = (1 == fwrite(&data_len, sizeof(int), 1, netfile));
    if(success)
        success = (data_len == fwrite(t->samples, sizeof(float), data_len, netfile));

    printf("sent data: ");
    for(i = 0; i < 5; i++)
        printf("%f,", t->samples[i]);
    printf("...\n");

    if(success)
    {
        if(cfg->ref_valid)
        {
            pattern_len = 0;
            success = (1 == fwrite(&pattern_len, sizeof(int), 1, netfile));
        }
        else
        {
            success = (1 == fwrite(&pattern_len, sizeof(int), 1, netfile));
            if(success)
                success = (pattern_len == fwrite(cfg->ref.match_pattern,
                                                 sizeof(float),
                                                 NUM_MATCH(&cfg->pattern), netfile));

            printf("sent pattern: ");
            for(i = 0; i < 5; i++)
                printf("%f,", cfg->ref.match_pattern[i]);
            printf("...\n");
        }
    }

    fflush(netfile);
    if(success)
        success = (res_len == fread(result, sizeof(float), res_len, netfile));
    if(!success)
    {
        err("Protocol error\n");
        goto __close_socket;
    }

    printf("received result: ");
    for(i = 0; i < 5; i++)
        printf("%f,", result[i]);
    printf("...\n");

    *res = result;
__close_socket:
    close(sockfd);
#else
    int i, ret, num;
    struct accumulator *acc;

    num = (int) t->owner->num_samples - NUM_MATCH(&cfg->pattern);
    ret = stat_create_dual_array(&acc, num, 1);
    if(ret < 0)
    {
        err("Failed to create accumulator for pattern\n");
        return ret;
    }

    for(i = 0; i < NUM_MATCH(&cfg->pattern); i++)
    {
        critical("Accumulating pattern %i\n", i);
        ret = stat_accumulate_dual_array(acc, &t->samples[i],
                                         &cfg->ref.match_pattern[i], num, 1);
        if(ret < 0)
        {
            err("Failed to accumulate\n");
            goto __free_accumulator;
        }
    }

    ret = stat_get_pearson_all(acc, res);
    if(ret < 0)
    {
        err("Failed to get pearson from accumulator\n");
        goto __free_accumulator;
    }

    ret = 0;
__free_accumulator:
    stat_free_accumulator(acc);
#endif

//    fwrite((*res), sizeof(float), t->owner->num_samples - NUM_MATCH(&cfg->pattern), stdout);
//
//    for(i = 0; i < t->owner->num_samples - NUM_MATCH(&cfg->pattern); i++)
//        printf("%f\n", (*res)[i]);
//    exit(0);

    return ret;
}

int __process_ref_trace(struct trace *t, struct tfm_extract_pattern_config *cfg)
{
    int ret, last_index;
    struct trace *ref_trace;
    struct split_list_entry *pos, *n;

    struct accumulator *acc;
    struct tfm_extract_pattern_block blk = {
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
    list_for_each_entry_safe(pos, n, &blk.split_list, list)
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
    struct tfm_extract_pattern_config *cfg = arg;
    struct tfm_extract_pattern_block *new;

    new = calloc(1, sizeof(struct tfm_extract_pattern_block));
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

int __allocate_debug_arrays(struct tfm_extract_pattern_block *blk)
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

int __send_debug(struct trace *t, struct tfm_extract_pattern_block *blk)
{
    int ret;
    char title[DEBUG_TITLE_SIZE];

    // Pearson correlation
    memset(title, 0, DEBUG_TITLE_SIZE * sizeof(char));
    snprintf(title, DEBUG_TITLE_SIZE, "Trace %i Pearson", blk->index);
    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_EXTRACT_PATTERN_DEBUG, 4,
                             5 * (blk->index) + 0,
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
                             5 * (blk->index) + 1,
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
                             5 * (blk->index) + 2,
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
                             5 * (blk->index) + 3,
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
                             5 * (blk->index) + 4,
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
    struct tfm_extract_pattern_config *cfg = arg;
    struct tfm_extract_pattern_block *blk = block;

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
            goto __out;
        }
    }

    // use pearson correlation to find true matches
    ret = __search_matches(cfg, blk);
    if(ret < 0)
    {
        err("Failed to search for matches\n");
        goto __free_debug;
    }

    debug("Trace %li: found %i true matches\n", TRACE_IDX(t), blk->count_true);

    if(blk->count_true == 0)
    {
        warn("No true matches in trace %li, discarding\n", TRACE_IDX(t));
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

    debug("Trace %li: found %i predictable gaps, %i unpredictable gaps\n",
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

    warn("Trace %li: C = %i, P = %i, U = %i, T = %i, total %i\n",
         TRACE_IDX(t), blk->count_true, blk->count_predictable, blk->count_unpredictable, blk->count_tail,
         blk->count_true + blk->count_predictable + blk->count_unpredictable + blk->count_tail);

    ret = 0;
__free_debug:
    if(ret < 0 && cfg->debugging)
    {
        free(blk->matches);
        free(blk->pred);
        free(blk->unpred);
        free(blk->tail);
    }

__out:
    if(ret < 0 || !cfg->debugging)
        free(blk->pearson);

    return ret;
}

int __increment_data(struct tfm_extract_pattern_config *cfg,
                     struct tfm_extract_pattern_block *blk)
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

int __finalize_data(struct tfm_extract_pattern_config *cfg,
                    struct tfm_extract_pattern_block *blk,
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
    uint8_t *data;

    struct tfm_extract_pattern_block *blk = block;
    struct tfm_extract_pattern_config *cfg = arg;
    struct split_list_entry *entry, *next;

    if(list_empty(&blk->split_list))
    {
        warn("Finalize called with empty split list\n");
        t->title = NULL;
        t->data = NULL;
        t->samples = NULL;
        goto __done;
    }

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

    entry = list_first_entry(&blk->split_list, struct split_list_entry, list);
    list_del(&entry->list);

    if(!(entry->type == SPLIT_CONFIDENT ||
         entry->type == SPLIT_GAP_TAIL))
    {
        err("List was left in incorrect position\n");
        return -EINVAL;
    }

    data = calloc(t->owner->data_size, sizeof(uint8_t));
    if(!data)
    {
        err("Failed to allocate data array\n");
        return -ENOMEM;
    }

    memcpy(data, blk->trace.data, t->owner->data_size);
    ret = __increment_data(cfg, blk);
    if(ret < 0)
    {
        err("Failed to increment data\n");
        goto __free_data;
    }

    ret = __finalize_data(cfg, blk, data);
    if(ret < 0)
    {
        err("Failed to finalize data\n");
        goto __free_data;
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
                        goto __free_data;
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
                goto __free_data;
        }
    }

    // let tfm_block fill title
    t->title = NULL;
    t->data = data;

    t->samples = calloc(cfg->pattern_size, sizeof(float));
    if(!t->samples)
    {
        err("Failed to allocate array for samples\n");
        ret = -ENOMEM;
        goto __free_data;
    }

    memcpy(t->samples, &blk->trace.samples[entry->index], cfg->pattern_size * sizeof(float));

__done:
    if(list_empty(&blk->split_list))
    {
        free(blk->trace.data);
        free(blk->trace.samples);
        free(blk);
        return 0;
    }
    else return 1;

__free_data:
    free(data);
    return ret;
}

int tfm_extract_pattern(struct tfm **tfm, int pattern_size, int expecting, int avg_len, int max_dev,
                        match_region_t *pattern, crypto_t data)
{
    int ret;
    struct tfm_extract_pattern_config *cfg;
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

    cfg = calloc(1, sizeof(struct tfm_extract_pattern_config));
    if(!cfg)
    {
        err("Failed to allocate config struct\n");
        return -ENOMEM;
    }

    ret = sem_init(&cfg->lock, 0, 1);
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
        sem_destroy(&cfg->lock);
        free(cfg);
        return ret;
    }

    return 0;
}