//#include "transform.h"
//#include "statistics.h"
//
//#include "__tfm_internal.h"
//
//#include "trace.h"
//#include "__trace_internal.h"
//
//#include <string.h>
//#include <errno.h>
//
//struct tfm_extract_pattern_config
//{
//    size_t ref_trace;
//    int lower, upper;
//    double confidence;
//
//    int expected, dist, dev;
//    float *match;
//};
//
//struct tfm_extract_pattern_block
//{
//    struct accumulator *gap_data;
//    float *pearson;
//    int *indices;
//};
//
//int tfm_extract_pattern_init(struct trace_set *ts, void *arg)
//{
//    struct tfm_extract_pattern_config *cfg = arg;
//
//    ts->data_size = ts->prev->data_size;
//    ts->num_samples = ts->prev->num_samples - (cfg->upper - cfg->lower);
//    return 0;
//}
//
//int tfm_extract_pattern_exit(struct trace_set *ts, void *arg)
//{
//    free(arg);
//    return 0;
//}
//
//int tfm_extract_pattern_initialize(struct trace *t, void **block, void *arg)
//{
//    int ret;
//    struct trace *prev;
//    struct tfm_extract_pattern_block *new;
//    struct tfm_extract_pattern_config *cfg = arg;
//
//    // this is safe to do since surrounding tfm_block synchronizes
//    if(!cfg->match)
//    {
//        ret = trace_get(t->owner, &prev, cfg->ref_trace);
//        if(ret < 0)
//        {
//            err("Failed to get reference trace\n");
//            return ret;
//        }
//
//        if(prev->samples)
//        {
//            cfg->match = calloc(cfg->upper - cfg->lower, sizeof(float));
//            if(!cfg->match)
//            {
//                err("Failed to allocate memory for reference pattern\n");
//                trace_free(prev);
//                return -ENOMEM;
//            }
//
//            memcpy(cfg->match, prev->samples,
//                   (cfg->upper - cfg->lower) * sizeof(float));
//            trace_free(prev);
//        }
//        else
//        {
//            err("Reference trace has no samples\n");
//            trace_free(prev);
//            return -EINVAL;
//        }
//    }
//
//    new = calloc(1, sizeof(struct tfm_extract_pattern_block));
//    if(!new)
//    {
//        err("Failed to allocate new block\n");
//        return -ENOMEM;
//    }
//
//    if(ret < 0)
//    {
//        err("Failed to create correlation accumulator\n");
//        goto __free_new;
//    }
//
//    ret = stat_create_single(&new->gap_data);
//    if(ret < 0)
//    {
//        err("Failed to create gap data accumulator\n");
//        goto __free_corr;
//    }
//
//    *block = new;
//    return 0;
//
//__free_corr:
//    stat_free_accumulator(new->correlation);
//
//__free_new:
//    free(new);
//    return ret;
//}
//
//bool tfm_extract_pattern_interesting(struct trace *t, void *arg)
//{
//    return (t->samples && t->data);
//}
//
//bool tfm_extract_pattern_matches(struct trace *t, void *block, void *arg)
//{
//    // each trace in its own block -- also enforced by DONE_SINGULAR
//    return false;
//}
//
//int tfm_extract_pattern_accumulate(struct trace *t, void *block, void *arg)
//{
//    int ret, i, num;
//    struct accumulator *acc;
//
//    struct tfm_extract_pattern_block *blk = block;
//    struct tfm_extract_pattern_config *cfg = arg;
//
//    cfg->
//
//    num = cfg->upper - cfg->lower;
//    ret = stat_create_dual_array(&acc, (int) t->owner->num_samples -
//                                 num, 1);
//    if(ret < 0)
//    {
//        err("Failed to create accumulator\n");
//        return ret;
//    }
//
//    for(i = 0; i < num; i++)
//    {
//        ret = stat_accumulate_dual_array(acc, &t->samples[i], &cfg->match[i],
//                                         (int) t->owner->num_samples - num, 1);
//        if(ret < 0)
//        {
//            err("Failed to accumulate\n");
//            goto __free_accumulator;
//        }
//    }
//
//    ret = stat_get_pearson_all(acc, &blk->pearson);
//    if(ret < 0)
//    {
//        err("Failed to get pearson values\n");
//        goto __free_accumulator;
//    }
//
//
//
//
//__free_accumulator:
//    stat_free_accumulator(acc);
//    return ret;
//}
//
//int tfm_extract_pattern_finalize(struct trace *t, void *block, void *arg)
//{
//    int ret;
//    struct tfm_extract_pattern_block *blk = block;
//    struct tfm_extract_pattern_config *cfg = arg;
//
//    return 0;
//}
//
//int tfm_extract_pattern(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param)
//{
//    int ret;
//    struct block_args block_args = {
//            .consumer_init = tfm_extract_pattern_init,
//            .consumer_exit = tfm_extract_pattern_exit,
//
//            .initialize = tfm_extract_pattern_initialize,
//            .trace_interesting = tfm_extract_pattern_interesting,
//            .trace_matches = tfm_extract_pattern_matches,
//            .accumulate = tfm_extract_pattern_accumulate,
//            .finalize = tfm_extract_pattern_finalize,
//
//            .criteria = DONE_LISTLEN
//    };
//
//    struct tfm_extract_pattern_config *cfg;
//    cfg = calloc(1, sizeof(struct tfm_extract_pattern_config));
//    if(!cfg)
//    {
//        err("Failed to allocate config struct\n");
//        return -ENOMEM;
//    }
//
//
//    ret = tfm_block(tfm, &block_args);
//    if(ret < 0)
//    {
//        err("Failed to initialize generic block transform\n");
//        return ret;
//    }
//
//    return 0;
//}