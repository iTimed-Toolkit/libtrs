#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <float.h>
#include <stdlib.h>
#include <math.h>

int __stat_reset_single(struct accumulator *acc)
{
    // this will cause overwrites on the next accumulate
    acc->count = 0;
    return 0;
}

int __stat_free_single(struct accumulator *acc)
{
    free(acc);
    return 0;
}

int __stat_get_single(struct accumulator *acc, stat_t stat, int index, float *res)
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
            val = acc->m.f; break;

        case STAT_DEV:
            val = sqrtf(acc->s.f / (acc->count - 1)); break;

        case STAT_MAX:
            val = acc->max.f; break;

        case STAT_MIN:
            val = acc->min.f; break;

        case STAT_MAXABS:
            val = acc->maxabs.f; break;

        case STAT_MINABS:
            val = acc->minabs.f; break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    *res = val;
    return 0;
}

int __stat_get_all_single(struct accumulator *acc, stat_t stat, float **res)
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

int stat_create_single(struct accumulator **acc, stat_t capabilities)
{
    struct accumulator *res;
    if(!acc)
    {
        err("Invalid destination pointer\n");
        return -EINVAL;
    }

    if(capabilities & (STAT_COV | STAT_PEARSON))
    {
        err("Covariance requested for single accumulator\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct accumulator));
    if(!res)
    {
        err("Failed to allocate accumulator\n");
        return -ENOMEM;
    }

    res->type = ACC_SINGLE;
    res->capabilities = capabilities;
    res->count = 0;

    res->reset = __stat_reset_single;
    res->free = __stat_free_single;
    res->get = __stat_get_single;
    res->get_all = __stat_get_all_single;

    *acc = res;
    return 0;
}

__attribute__ ((always_inline))
static inline int __accumulate_single(struct accumulator *acc, float val)
{
    float m_new;

    acc->count++;
    if(acc->count == 1)
    {
        IF_CAP(acc, STAT_AVG | STAT_DEV) acc->m.f = val;
        IF_CAP(acc, STAT_DEV) acc->s.f = 0;
        IF_CAP(acc, STAT_MAX) acc->max.f = val;
        IF_CAP(acc, STAT_MIN) acc->min.f = val;
        IF_CAP(acc, STAT_MAXABS) acc->maxabs.f = fabsf(val);
        IF_CAP(acc, STAT_MINABS) acc->minabs.f = fabsf(val);
    }
    else
    {
        IF_CAP(acc, STAT_AVG | STAT_DEV) m_new = acc->m.f + (val - acc->m.f) / acc->count;
        IF_CAP(acc, STAT_DEV) acc->s.f += ((val - acc->m.f) * (val - m_new));
        IF_CAP(acc, STAT_AVG | STAT_DEV) acc->m.f = m_new;

        IF_CAP(acc, STAT_MAX) { if(val > acc->max.f) acc->max.f = val; }
        IF_CAP(acc, STAT_MIN) { if(val < acc->min.f) acc->min.f = val; }
        IF_CAP(acc, STAT_MAXABS) { if(fabsf(val) > acc->maxabs.f) acc->maxabs.f = fabsf(val); }
        IF_CAP(acc, STAT_MINABS) { if(fabsf(val) < acc->minabs.f) acc->minabs.f = fabsf(val); }
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