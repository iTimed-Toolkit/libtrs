#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>

int __stat_reset_dual(struct accumulator *acc)
{
    acc->count = 0;
    CAP_RESET_ARRAY(acc, _AVG, 0, 2);
    CAP_RESET_ARRAY(acc, _DEV, 0, 2);
    CAP_RESET_SCALAR(acc, _COV, 0.0f);
    CAP_RESET_ARRAY(acc, _MAX, 0, 2);
    CAP_RESET_ARRAY(acc, _MIN, 0, 2);
    CAP_RESET_ARRAY(acc, _MAXABS, 0, 2);
    CAP_RESET_ARRAY(acc, _MINABS, 0, 2);
    return 0;
}

int __stat_free_dual(struct accumulator *acc)
{
    CAP_FREE_ARRAY(acc, _AVG);
    CAP_FREE_ARRAY(acc, _DEV);
    CAP_FREE_ARRAY(acc, _MAX);
    CAP_FREE_ARRAY(acc, _MIN);
    CAP_FREE_ARRAY(acc, _MAXABS);
    CAP_FREE_ARRAY(acc, _MINABS);
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

    if(stat & (STAT_COV | STAT_PEARSON) && index != 0)
    {
        err("Invalid index for accumulator and statistic\n");
        return -EINVAL;
    }
    else if(index >= 2)
    {
        err("Invalid index for accumulator and statistic\n");
        return -EINVAL;
    }

    switch(stat)
    {
        case STAT_AVG:
            val = acc->_AVG.a[index]; break;

        case STAT_DEV:
            val = sqrtf(acc->_DEV.a[index] / (acc->count - 1)); break;

        case STAT_COV:
            val = acc->_COV.f; break;

        case STAT_PEARSON:
            val = acc->_COV.f /
                  ((acc->count - 1) *
                   sqrtf(acc->_DEV.a[0] / (acc->count - 1)) *
                   sqrtf(acc->_DEV.a[1] / (acc->count - 1)));
            break;

        case STAT_MAX:
            val = acc->_MAX.a[index]; break;

        case STAT_MIN:
            val = acc->_MIN.a[index]; break;

        case STAT_MAXABS:
            val = acc->_MAXABS.a[index]; break;

        case STAT_MINABS:
            val = acc->_MINABS.a[index]; break;

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
    float *result;

    if(stat & (STAT_COV | STAT_PEARSON))
        result = calloc(1, sizeof(float));
    else
        result = calloc(2, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    if(stat & (STAT_COV | STAT_PEARSON))
    {
        ret = __stat_get_dual(acc, stat, 0, &result[0]);
        if(ret < 0)
        {
            err("Failed to get statistic\n");
            free(result);
            return ret;
        }
    }
    else
    {
        ret = __stat_get_dual(acc, stat, 0, &result[0]);
        if(ret < 0)
        {
            err("Failed to get statistic\n");
            free(result);
            return ret;
        }

        ret = __stat_get_dual(acc, stat, 1, &result[1]);
        if(ret < 0)
        {
            err("Failed to get statistic\n");
            free(result);
            return ret;
        }
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

    CAP_INIT_ARRAY(res, _AVG, 2, __free_acc);
    CAP_INIT_ARRAY(res, _DEV, 2, __free_acc);
    CAP_INIT_SCALAR(res, _COV, 0);
    CAP_INIT_ARRAY(res, _MAX, 2, __free_acc);
    CAP_INIT_ARRAY(res, _MIN, 2, __free_acc);
    CAP_INIT_ARRAY(res, _MAXABS, 2, __free_acc);
    CAP_INIT_ARRAY(res, _MINABS, 2, __free_acc);

    res->reset = __stat_reset_dual;
    res->free = __stat_free_dual;
    res->get = __stat_get_dual;
    res->get_all = __stat_get_all_dual;

    *acc = res;
    return 0;

__free_acc:
    CAP_FREE_ARRAY(res, _AVG);
    CAP_FREE_ARRAY(res, _DEV);
    CAP_FREE_ARRAY(res, _MAX);
    CAP_FREE_ARRAY(res, _MIN);
    CAP_FREE_ARRAY(res, _MAXABS);
    CAP_FREE_ARRAY(res, _MINABS);
    free(res);
    return -ENOMEM;
}

#if defined(LIBTRACE_PLATFORM_LINUX)
__attribute__((always_inline)) static inline
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
static __forceinline
#endif
int __accumulate_dual(struct accumulator *acc, float val0, float val1)
{
    float m_new_0, m_new_1;

    acc->count++;
    if(acc->count == 1)
    {
        IF_CAP(acc, _AVG)
        {
            acc->_AVG.a[0] = val0;
            acc->_AVG.a[1] = val1;
        }

        IF_CAP(acc, _DEV)
        {
            acc->_DEV.a[0] = 0;
            acc->_DEV.a[1] = 0;
        }

        IF_CAP(acc, _COV) acc->_COV.f = 0;

        IF_CAP(acc, _MAX)
        {
            acc->_MAX.a[0] = val0;
            acc->_MAX.a[1] = val1;
        }

        IF_CAP(acc, _MIN)
        {
            acc->_MIN.a[0] = val0;
            acc->_MIN.a[1] = val1;
        }

        IF_CAP(acc, _MAXABS)
        {
            acc->_MAXABS.a[0] = fabsf(val0);
            acc->_MAXABS.a[1] = fabsf(val1);
        }

        IF_CAP(acc, _MINABS)
        {
            acc->_MINABS.a[0] = fabsf(val0);
            acc->_MINABS.a[1] = fabsf(val1);
        }
    }
    else
    {
        IF_CAP(acc, _AVG)
        {
            m_new_0 = acc->_AVG.a[0] + (val0 - acc->_AVG.a[0]) / acc->count;
            m_new_1 = acc->_AVG.a[1] + (val1 - acc->_AVG.a[1]) / acc->count;
            acc->_DEV.a[0] += ((val0 - acc->_AVG.a[0]) * (val0 - m_new_0));
            acc->_DEV.a[1] += ((val1 - acc->_AVG.a[1]) * (val1 - m_new_1));

            IF_CAP(acc, _COV)
            { acc->_COV.f += ((val0 - acc->_AVG.a[0]) * (val1 - m_new_1)); }

            acc->_AVG.a[0] = m_new_0;
            acc->_AVG.a[1] = m_new_1;
        }

        IF_CAP(acc, _MAX)
        {
            acc->_MAX.a[0] = (val0 > acc->_MAX.a[0] ? val0 : acc->_MAX.a[0]);
            acc->_MAX.a[1] = (val1 > acc->_MAX.a[1] ? val1 : acc->_MAX.a[1]);
        }

        IF_CAP(acc, _MIN)
        {
            acc->_MIN.a[0] = (val0 < acc->_MIN.a[0] ? val0 : acc->_MIN.a[0]);
            acc->_MIN.a[1] = (val1 < acc->_MIN.a[1] ? val1 : acc->_MIN.a[1]);
        }

        IF_CAP(acc, _MAXABS)
        {
            acc->_MAXABS.a[0] = (fabsf(val0) > acc->_MAXABS.a[0] ?
                                fabsf(val0) : acc->_MAXABS.a[0]);
            acc->_MAXABS.a[1] = (fabsf(val1) > acc->_MAXABS.a[1] ?
                                fabsf(val1) : acc->_MAXABS.a[1]);
        }

        IF_CAP(acc, _MINABS)
        {
            acc->_MINABS.a[0] = (fabsf(val0) < acc->_MINABS.a[0] ?
                                fabsf(val0) : acc->_MINABS.a[0]);
            acc->_MINABS.a[1] = (fabsf(val1) < acc->_MINABS.a[1] ?
                                fabsf(val1) : acc->_MINABS.a[1]);
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