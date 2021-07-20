#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>

int stat_reset_accumulator(struct accumulator *acc)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    return acc->reset(acc);
}

int stat_free_accumulator(struct accumulator *acc)
{
    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    acc->free(acc);
    free(acc);
    return 0;
}

int stat_get(struct accumulator *acc, stat_t stat, int index, float *res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or destination pointer\n");
        return -EINVAL;
    }

    if(acc->get)
        return acc->get(acc, stat, index, res);
    else
    {
        err("Accumulator does not have a get function\n");
        return -EINVAL;
    }
}

int stat_get_all(struct accumulator *acc, stat_t stat, float **res)
{
    if(!acc || !res)
    {
        err("Invalid accumulator or destination pointer\n");
        return -EINVAL;
    }

    if(acc->get_all)
        return acc->get_all(acc, stat, res);
    else
    {
        err("Accumulator does not have a get all function\n");
        return -EINVAL;
    }
}