#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>

int stat_create_single(struct accumulator **acc)
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

    res->type = ACC_SINGLE;
    res->count = 0;
    res->m.f = 0;
    res->s.f = 0;
    res->cov.f = 0;

    *acc = res;
    return 0;
}

int __accumulate_single(struct accumulator *acc, float val)
{
    float m_new;

    acc->count++;
    if(acc->count == 1)
    {
        acc->m.f = val;
        acc->s.f = 0;
    }
    else
    {
        m_new = acc->m.f + (val - acc->m.f) / acc->count;
        acc->s.f += ((val - acc->m.f) * (val - m_new));
        acc->m.f = m_new;
    }

    return 0;
}

int stat_accumulate_single(struct accumulator *acc, float val)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->type != ACC_SINGLE)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    return __accumulate_single(acc, val);
}

int stat_accumulate_single_many(struct accumulator *acc, float *val, int num)
{
    int i, ret;
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->type != ACC_SINGLE)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    for(i = 0; i < num; i++)
    {
        ret = __accumulate_single(acc, val[i]);
        if(ret < 0)
        {
            err("Failed to accumulate at index %i\n", i);
            return ret;
        }
    }

    return 0;
}

int __get_mean_single(struct accumulator *acc, int index, float *res)
{
    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->m.f;
    return 0;
}

int __get_dev_single(struct accumulator *acc, int index, float *res)
{
    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = sqrtf(acc->s.f / (acc->count - 1));
    return 0;
}

int __get_mean_single_all(struct accumulator *acc, float **res)
{
    int ret;
    float *result = calloc(1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    ret = __get_mean_single(acc, 0, result);
    if(ret < 0)
    {
        free(result);
        return ret;
    }

    *res = result;
    return 0;
}

int __get_dev_single_all(struct accumulator *acc, float **res)
{
    int ret;
    float *result = calloc(1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    ret = __get_dev_single(acc, 0, result);
    if(ret < 0)
    {
        free(result);
        return ret;
    }

    *res = result;
    return 0;
}