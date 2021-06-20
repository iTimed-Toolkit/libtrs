#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>

int __stat_reset_dual(struct accumulator *acc)
{
    acc->count = 0;

    IF_CAP(acc, STAT_AVG | STAT_DEV | STAT_COV | STAT_PEARSON)
    { memset(acc->m.a, 0, 2 * sizeof(float)); }

    IF_CAP(acc, STAT_DEV)
    { memset(acc->s.a, 0, 2 * sizeof(float)); }

    IF_CAP(acc, STAT_COV | STAT_PEARSON) acc->cov.f = 0;

    IF_CAP(acc, STAT_MAX)
    { memset(acc->max.a, 0, 2 * sizeof(float)); }

    IF_CAP(acc, STAT_MIN)
    { memset(acc->min.a, 0, 2 * sizeof(float)); }

    IF_CAP(acc, STAT_MAXABS)
    { memset(acc->maxabs.a, 0, 2 * sizeof(float)); }

    IF_CAP(acc, STAT_MINABS)
    { memset(acc->minabs.a, 0, 2 * sizeof(float)); }

    return 0;
}

int __stat_free_dual(struct accumulator *acc)
{
    if(acc->m.a)
        free(acc->m.a);

    if(acc->s.a)
        free(acc->s.a);

    if(acc->max.a)
        free(acc->max.a);

    if(acc->min.a)
        free(acc->min.a);

    if(acc->maxabs.a)
        free(acc->maxabs.a);

    if(acc->minabs.a)
        free(acc->minabs.a);

    return 0;
}

int __stat_get_dual(struct accumulator *acc, stat_t stat, int index, float *res)
{
    float val;

    IF_NOT_CAP(acc, stat)
    {
        err("Accumulator does not have requested capability");
        return -EINVAL;
    }

    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    switch(stat)
    {
        case STAT_AVG:
            val = acc->m.f;
            break;

        case STAT_DEV:
            val = sqrtf(acc->s.f / (acc->count - 1));
            break;

        case STAT_MAX:
            val = acc->max.f;
            break;

        case STAT_MIN:
            val = acc->min.f;
            break;

        case STAT_MAXABS:
            val = acc->maxabs.f;
            break;

        case STAT_MINABS:
            val = acc->minabs.f;
            break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    *res = val;
    return 0;
}

int __stat_get_all_dual(struct accumulator *acc, stat_t stat, float **res)
{
    int ret;
    float *result = calloc(1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    ret = __stat_get_single(acc, stat, 0, result);
    if(ret < 0)
    {
        err("Failed to get statistic\n");
        free(result);
        return ret;
    }

    *res = result;
    return 0;
}

int stat_create_dual(struct accumulator **acc, stat_t capabilities)
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

    res->type = ACC_DUAL;
    res->capabilities = capabilities;
    res->count = 0;

    IF_CAP(res, STAT_AVG | STAT_DEV |
                STAT_COV | STAT_PEARSON)
    {
        res->m.a = calloc(2, sizeof(float));
        if(!res->m.a)
        {
            err("Failed to allocate dual m array\n");
            goto __free_acc;
        }
    }

    IF_CAP(res, STAT_DEV)
    {
        res->s.a = calloc(2, sizeof(float));
        if(!res->s.a)
        {
            err("Failed to allocate dual s array\n");
            goto __free_acc;
        }
    }

    IF_CAP(res, STAT_COV | STAT_PEARSON) res->cov.f = 0;

    IF_CAP(res, STAT_MAX)
    {
        res->max.a = calloc(2, sizeof(float));
        if(!res->max.a)
        {
            err("Failed to allocate dual max array\n");
            goto __free_acc;
        }
    }

    IF_CAP(res, STAT_MIN)
    {
        res->min.a = calloc(2, sizeof(float));
        if(!res->min.a)
        {
            err("Failed to allocate dual min array\n");
            goto __free_acc;
        }
    }

    IF_CAP(res, STAT_MAXABS)
    {
        res->maxabs.a = calloc(2, sizeof(float));
        if(!res->maxabs.a)
        {
            err("Failed to allocate dual maxabs array\n");
            goto __free_acc;
        }
    }

    IF_CAP(res, STAT_MINABS)
    {
        res->minabs.a = calloc(2, sizeof(float));
        if(!res->minabs.a)
        {
            err("Failed to allocate dual minabs array\n");
            goto __free_acc;
        }
    }

    res->reset = __stat_reset_dual;
    res->free = __stat_free_dual;
    res->get = __stat_get_dual;
    res->get_all = __stat_get_all_dual;

    *acc = res;
    return 0;

__free_acc:
    if(res->m.a)
        free(res->m.a);

    if(res->s.a)
        free(res->s.a);

    if(res->max.a)
        free(res->max.a);

    if(res->min.a)
        free(res->min.a);

    if(res->maxabs.a)
        free(res->maxabs.a);

    if(res->minabs.a)
        free(res->minabs.a);

    free(res);
    return -ENOMEM;
}

__attribute__ ((always_inline))
static inline int __accumulate_dual(struct accumulator *acc, float val0, float val1)
{
    float m_new_0, m_new_1;

    acc->count++;
    if(acc->count == 1)
    {
        IF_CAP(acc, STAT_AVG | STAT_DEV |
                    STAT_COV | STAT_PEARSON)
        {
            acc->m.a[0] = val0;
            acc->m.a[1] = val1;
        }

        IF_CAP(acc, STAT_DEV)
        {
            acc->s.a[0] = 0;
            acc->s.a[1] = 0;
        }

        IF_CAP(acc, STAT_COV | STAT_PEARSON) acc->cov.f = 0;

        IF_CAP(acc, STAT_MAX)
        {
            acc->max.a[0] = val0;
            acc->max.a[1] = val1;
        }

        IF_CAP(acc, STAT_MIN)
        {
            acc->min.a[0] = val0;
            acc->min.a[1] = val1;
        }

        IF_CAP(acc, STAT_MAXABS)
        {
            acc->maxabs.a[0] = fabsf(val0);
            acc->maxabs.a[1] = fabsf(val1);
        }

        IF_CAP(acc, STAT_MINABS)
        {
            acc->minabs.a[0] = fabsf(val0);
            acc->minabs.a[1] = fabsf(val1);
        }
    }
    else
    {
        IF_CAP(acc, STAT_AVG | STAT_DEV |
                    STAT_COV | STAT_PEARSON)
        {
            m_new_0 = acc->m.a[0] + (val0 - acc->m.a[0]) / acc->count;
            m_new_1 = acc->m.a[1] + (val1 - acc->m.a[1]) / acc->count;
        }

        IF_CAP(acc, STAT_DEV)
        {
            acc->s.a[0] += ((val0 - acc->m.a[0]) * (val0 - m_new_0));
            acc->s.a[1] += ((val1 - acc->m.a[1]) * (val1 - m_new_1));
        }

        IF_CAP(acc, STAT_COV | STAT_PEARSON)
        { acc->cov.f += ((val0 - acc->m.a[0]) * (val1 - m_new_1)); }

        IF_CAP(acc, STAT_AVG | STAT_DEV |
                    STAT_COV | STAT_PEARSON)
        {
            acc->m.a[0] = m_new_0;
            acc->m.a[1] = m_new_1;
        }

        IF_CAP(acc, STAT_MAX)
        {
            acc->max.a[0] = (val0 > acc->max.a[0] ? val0 : acc->max.a[0]);
            acc->max.a[1] = (val1 > acc->max.a[1] ? val1 : acc->max.a[1]);
        }

        IF_CAP(acc, STAT_MIN)
        {
            acc->min.a[0] = (val0 < acc->max.a[0] ? val0 : acc->min.a[0]);
            acc->min.a[1] = (val1 < acc->max.a[1] ? val1 : acc->min.a[1]);
        }

        IF_CAP(acc, STAT_MAXABS)
        {
            acc->maxabs.a[0] = (fabsf(val0) > acc->maxabs.a[0] ?
                                fabsf(val0) : acc->maxabs.a[0]);
            acc->maxabs.a[1] = (fabsf(val1) > acc->maxabs.a[1] ?
                                fabsf(val1) : acc->maxabs.a[1]);
        }

        IF_CAP(acc, STAT_MINABS)
        {
            acc->minabs.a[0] = (fabsf(val0) < acc->minabs.a[0] ?
                                fabsf(val0) : acc->minabs.a[0]);
            acc->minabs.a[1] = (fabsf(val1) < acc->minabs.a[1] ?
                                fabsf(val1) : acc->minabs.a[1]);
        }
    }

    return 0;
}

int stat_accumulate_dual(struct accumulator *acc, float val0, float val1)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->type != ACC_DUAL)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    return __accumulate_dual(acc, val0, val1);
}

int stat_accumulate_dual_many(struct accumulator *acc, float *val0, float *val1, int num)
{
    int i, ret;
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->type != ACC_DUAL)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    for(i = 0; i < num; i++)
    {
        ret = __accumulate_dual(acc, val0[i], val1[i]);
        if(ret < 0)
        {
            err("Failed to accumulate at index %i\n", i);
            return ret;
        }
    }

    return 0;
}

int __get_mean_dual(struct accumulator *acc, int index, float *res)
{
    if(index >= 2)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->m.a[index];
    return 0;
}

int __get_dev_dual(struct accumulator *acc, int index, float *res)
{
    if(index >= 2)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = sqrtf(acc->s.a[index] / (acc->count - 1));
    return 0;
}

int __get_cov_dual(struct accumulator *acc, int index, float *res)
{
    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->cov.f / acc->count;
    return 0;
}

int __get_pearson_dual(struct accumulator *acc, int index, float *res)
{
    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->cov.f /
           ((acc->count - 1) *
            sqrtf(acc->s.a[0] / (acc->count - 1)) *
            sqrtf(acc->s.a[1] / (acc->count - 1)));
    return 0;
}

int __get_mean_dual_all(struct accumulator *acc, float **res)
{
    float *result = calloc(2, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    result[0] = acc->m.a[0];
    result[1] = acc->m.a[1];
    *res = result;
    return 0;
}

int __get_dev_dual_all(struct accumulator *acc, float **res)
{
    float *result = calloc(2, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    result[0] = sqrtf(acc->s.a[0] / (acc->count - 1));
    result[1] = sqrtf(acc->s.a[1] / (acc->count - 1));
    *res = result;
    return 0;
}

int __get_cov_dual_all(struct accumulator *acc, float **res)
{
    int ret;
    float *result = calloc(1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    ret = __get_cov_dual(acc, 0, result);
    if(ret < 0)
    {
        free(result);
        return ret;
    }

    *res = result;
    return 0;
}

int __get_pearson_dual_all(struct accumulator *acc, float **res)
{
    int ret;
    float *result = calloc(1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    ret = __get_pearson_dual(acc, 0, result);
    if(ret < 0)
    {
        free(result);
        return ret;
    }

    *res = result;
    return 0;
}
