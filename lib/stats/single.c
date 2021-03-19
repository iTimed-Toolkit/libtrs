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

    res->acc_type = ACC_SINGLE;
    res->acc_count = 0;
    res->acc_m.f = 0;
    res->acc_s.f = 0;
    res->acc_cov.f = 0;

    *acc = res;
    return 0;
}

int stat_accumulate_single(struct accumulator *acc, float val)
{
    float m_new;

    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->acc_type != ACC_SINGLE)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    acc->acc_count++;
    if(acc->acc_count == 1)
    {
        acc->acc_m.f = val;
        acc->acc_s.f = 0;
    }
    else
    {
        m_new = acc->acc_m.f + (val - acc->acc_m.f) / acc->acc_count;
        acc->acc_s.f += ((val - acc->acc_m.f) * (val - m_new));
        acc->acc_m.f = m_new;
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

    *res = acc->acc_m.f;
    return 0;
}

int __get_dev_single(struct accumulator *acc, int index, float *res)
{
    if(index != 0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = sqrtf(acc->acc_s.f / (acc->acc_count - 1));
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