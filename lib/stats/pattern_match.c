#include "statistics.h"
#include "__stat_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>

int __stat_reset_pattern_match(struct accumulator *acc)
{
    // nothing to do
    return 0;
}

int __stat_free_pattern_match(struct accumulator *acc)
{
#if USE_GPU
    return gpu_pattern_free(acc->_AVG.a);
#else
    CAP_FREE_ARRAY(acc, _AVG);
    return 0;
#endif
}

int stat_create_pattern_match(struct accumulator **acc, float *pattern, int pattern_len, int match_len)
{
    struct accumulator *res;
    if(!acc)
    {
        err("Invalid destination pointer\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct accumulator));
    if(!res)
    {
        err("Failed to allocate accumulator\n");
        return -ENOMEM;
    }

    res->type = ACC_PATTERN_MATCH;
    res->capabilities = STAT_PEARSON;
    res->dim0 = pattern_len;
    res->dim1 = match_len;
    res->count = 0;

#if USE_GPU
    int ret = gpu_pattern_preprocess(pattern, pattern_len, &res->_AVG.a, &res->_DEV.f);
    if(ret < 0)
    {
        err("Failed to preprocess GPU pattern\n");
        goto __free_acc;
    }
#else
    CAP_INIT_ARRAY(res, _AVG, pattern_len, __free_acc);
    memcpy(res->_AVG.a, pattern, pattern_len * sizeof(float));
#endif

    res->reset = __stat_reset_pattern_match;
    res->free = __stat_free_pattern_match;
    res->get = NULL;
    res->get_all = NULL;

    *acc = res;
    return 0;

__free_acc:
    free(res);
    return -ENOMEM;
}

int stat_pattern_match(struct accumulator *acc, float *match, int match_len, float **res)
{
    if(match_len != acc->dim1)
    {
        err("Invalid data size\n");
        return -EINVAL;
    }

#if USE_GPU
    return gpu_pattern_match(match, match_len, acc->_AVG.a, acc->dim0, acc->_DEV.f, res);
#else
    int ret, i;
    struct accumulator *acc_pearson;

    ret = stat_create_dual_array(&acc_pearson, STAT_PEARSON, acc->dim1 - acc->dim0, 1);
    if(ret < 0)
    {
        err("Failed to create accumulator for pattern match\n");
        return ret;
    }

    for(i = 0; i < acc->dim0; i++)
    {
        warn("%i\n", i);
        ret = stat_accumulate_dual_array(acc_pearson, &match[i],
                                         &acc->_AVG.a[i], acc->dim1 - acc->dim0, 1);
        if(ret < 0)
        {
            err("Failed to accumulate\n");
            goto __free_accumulator;
        }
    }

    ret = stat_get_all(acc_pearson, STAT_PEARSON, res);
    if(ret < 0)
    {
        err("Failed to get pearson from accumulator\n");
        goto __free_accumulator;
    }

    ret = 0;
__free_accumulator:
    stat_free_accumulator(acc_pearson);
    return ret;
#endif
}