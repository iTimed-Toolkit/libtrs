#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>

int stat_create_dual(struct accumulator **acc)
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
    res->count = 0;

    res->m.a = calloc(2, sizeof(float));
    if(!res->m.a)
    {
        err("Failed to allocate dual m array\n");
        goto __free_acc;
    }

    res->s.a = calloc(2, sizeof(float));
    if(!res->s.a)
    {
        err("Failed to allocate dual s array\n");
        goto __free_acc;
    }

    res->cov.f = 0;

    *acc = res;
    return 0;

__free_acc:
    if(res->m.a)
        free(res->m.a);

    if(res->s.a)
        free(res->s.a);

    free(res);
    return -ENOMEM;
}

int __accumulate_dual(struct accumulator *acc, float val0, float val1)
{
    float m_new_0, m_new_1;

    acc->count++;
    if(acc->count == 1)
    {
        acc->m.a[0] = val0;
        acc->m.a[1] = val1;

        acc->s.a[0] = 0;
        acc->s.a[1] = 0;
        acc->cov.f = 0;
    }
    else
    {
        m_new_0 = acc->m.a[0] + (val0 - acc->m.a[0]) / acc->count;
        m_new_1 = acc->m.a[1] + (val1 - acc->m.a[1]) / acc->count;

        acc->s.a[0] += ((val0 - acc->m.a[0]) * (val0 - m_new_0));
        acc->s.a[1] += ((val1 - acc->m.a[1]) * (val1 - m_new_1));
        acc->cov.f += ((val0 - acc->m.a[0]) * (val1 - m_new_1));
        acc->m.a[0] = m_new_0;
        acc->m.a[1] = m_new_1;
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