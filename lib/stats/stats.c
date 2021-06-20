#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <string.h>

int stat_reset_accumulator(struct accumulator *acc)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(!acc->reset)
    {
        err("Invalid accumulator reset function\n");
        return -EINVAL;
    }

    return acc->reset(acc);

    switch(acc->type)
    {

        case ACC_SINGLE_ARRAY:
            acc->count = 0;
            memset(acc->m.a, 0, acc->dim0 * sizeof(float));
            memset(acc->s.a, 0, acc->dim0 * sizeof(float));
            acc->cov.f = 0;
            return 0;


        case ACC_DUAL_ARRAY:
            acc->count = 0;
            memset(acc->m.a, 0, (acc->dim0 + acc->dim1) * sizeof(float));
            memset(acc->s.a, 0, (acc->dim0 + acc->dim1) * sizeof(float));
            memset(acc->cov.a, 0, (acc->dim0 * acc->dim1) * sizeof(float));
            return 0;

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_free_accumulator(struct accumulator *acc)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(!acc->free)
    {
        err("Invalid accumulator free function\n");
        return -EINVAL;
    }

    return acc->free(acc);

    switch(acc->type)
    {
        case ACC_DUAL_ARRAY:
            free(acc->cov.a);

        case ACC_DUAL:
        case ACC_SINGLE_ARRAY:
            free(acc->m.a);
            free(acc->s.a);

        case ACC_SINGLE:
            free(acc);
            return 0;

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get(struct accumulator *acc, stat_t stat, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or destination pointer\n");
        return -EINVAL;
    }

    return acc->get(acc, stat, index, res);
}

int stat_get_all(struct accumulator *acc, stat_t stat, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or destination pointer\n");
        return -EINVAL;
    }

    return acc->get_all(acc, stat, res);
}