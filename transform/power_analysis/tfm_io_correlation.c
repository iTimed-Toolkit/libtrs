#include "transform.h"
#include "__tfm_internal.h"

#include "__libtrs_internal.h"

#include <errno.h>
#include <string.h>

static inline uint8_t hamming_weight(uint8_t n)
{
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);
    return n;
}

static inline float pm_generic(uint8_t *data, int index, int div)
{
    int i;
    float sum = 0;

    for(i = 0; i < div; i++)
        sum += (float) hamming_weight(data[div * index + i]);

    return sum;
}

float pm_8bit(uint8_t *data, int index)
{
    return (float) hamming_weight(data[index]);
}

float pm_16bit(uint8_t *data, int index)
{
    return pm_generic(data, index, 2);
}

float pm_32bit(uint8_t *data, int index)
{
    return pm_generic(data, index, 4);
}

float pm_64bit(uint8_t *data, int index)
{
    return pm_generic(data, index, 8);
}

struct tfm_io_correlation_arg
{
    int granularity;
    int num;
};

int tfm_io_correlation_init(struct trace_set *ts, void *arg)
{
    struct tfm_io_correlation_arg *io_arg = arg;

    if(!ts || !arg)
    {
        err("Invalid trace set or init arg\n");
        return -EINVAL;
    }

    ts->num_traces = io_arg->num;
    return 0;
}

int tfm_io_correlation(struct tfm **tfm, int granularity, int num)
{
    int ret;
    struct tfm_io_correlation_arg *arg;

    arg = calloc(1, sizeof(struct tfm_io_correlation_arg));
    if(!arg)
    {
        err("Failed to allocate CPA arg\n");
        return -ENOMEM;
    }

    arg->granularity = granularity;
    arg->num = num;

    switch(granularity)
    {
        case 8:
            ret = tfm_cpa(tfm, pm_8bit,
                          tfm_io_correlation_init, arg);
            break;

        case 16:
            ret = tfm_cpa(tfm, pm_16bit,
                          tfm_io_correlation_init, arg);
            break;

        case 32:
            ret = tfm_cpa(tfm, pm_32bit,
                          tfm_io_correlation_init, arg);
            break;

        case 64:
            ret = tfm_cpa(tfm, pm_64bit,
                          tfm_io_correlation_init, arg);
            break;

        default:
            err("Unsupported granularity\n");
            return -EINVAL;
    }

    if(ret < 0)
    {
        err("Failed to initialize generic CPA transform\n");
        return ret;
    }

    return 0;
}