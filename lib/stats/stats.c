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

    switch(acc->type)
    {
        case ACC_SINGLE:
            acc->count = 0;
            acc->m.f = 0;
            acc->s.f = 0;
            acc->cov.f = 0;
            return 0;

        case ACC_SINGLE_ARRAY:
            acc->count = 0;
            memset(acc->m.a, 0, acc->dim0 * sizeof(float));
            memset(acc->s.a, 0, acc->dim0 * sizeof(float));
            acc->cov.f = 0;
            return 0;

        case ACC_DUAL:
            acc->count = 0;
            memset(acc->m.a, 0, 2 * sizeof(float));
            memset(acc->s.a, 0, 2 * sizeof(float));
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

int stat_get_mean(struct accumulator *acc, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
            return __get_mean_single(acc, index, res);

        case ACC_DUAL:
            return __get_mean_dual(acc, index, res);

        case ACC_SINGLE_ARRAY:
            return __get_mean_single_array(acc, index, res);

        case ACC_DUAL_ARRAY:
            return __get_mean_dual_array(acc, index, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_dev(struct accumulator *acc, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
            return __get_dev_single(acc, index, res);

        case ACC_DUAL:
            return __get_dev_dual(acc, index, res);

        case ACC_SINGLE_ARRAY:
            return __get_dev_single_array(acc, index, res);

        case ACC_DUAL_ARRAY:
            return __get_dev_dual_array(acc, index, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_cov(struct accumulator *acc, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
        case ACC_SINGLE_ARRAY:
            err("Single accumulators have no covariance\n");
            return -EINVAL;

        case ACC_DUAL:
            return __get_cov_dual(acc, index, res);

        case ACC_DUAL_ARRAY:
            return __get_cov_dual_array(acc, index, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_pearson(struct accumulator *acc, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
        case ACC_SINGLE_ARRAY:
            err("Single accumulators have no pearson\n");
            return -EINVAL;

        case ACC_DUAL:
            return __get_pearson_dual(acc, index, res);

        case ACC_DUAL_ARRAY:
            return __get_pearson_dual_array(acc, index, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_mean_all(struct accumulator *acc, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
            return __get_mean_single_all(acc, res);

        case ACC_DUAL:
            return __get_mean_dual_all(acc, res);

        case ACC_SINGLE_ARRAY:
            return __get_mean_single_array_all(acc, res);

        case ACC_DUAL_ARRAY:
            return __get_mean_dual_array_all(acc, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_dev_all(struct accumulator *acc, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
            return __get_dev_single_all(acc, res);

        case ACC_DUAL:
            return __get_dev_dual_all(acc, res);

        case ACC_SINGLE_ARRAY:
            return __get_dev_single_array_all(acc, res);

        case ACC_DUAL_ARRAY:
            return __get_dev_dual_array_all(acc, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_cov_all(struct accumulator *acc, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
        case ACC_SINGLE_ARRAY:
            err("Single accumulators have no pearson\n");
            return -EINVAL;

        case ACC_DUAL:
            return __get_cov_dual_all(acc, res);

        case ACC_DUAL_ARRAY:
            return __get_cov_dual_array_all(acc, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

int stat_get_pearson_all(struct accumulator *acc, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or result destination\n");
        return -EINVAL;
    }

    switch(acc->type)
    {
        case ACC_SINGLE:
        case ACC_SINGLE_ARRAY:
            err("Single accumulators have no pearson\n");
            return -EINVAL;

        case ACC_DUAL:
            return __get_pearson_dual_all(acc, res);

        case ACC_DUAL_ARRAY:
            return __get_pearson_dual_array_all(acc, res);

        default:
            err("Unknown accumulator type\n");
            return -EINVAL;
    }
}

