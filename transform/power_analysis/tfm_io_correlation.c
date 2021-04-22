#include "transform.h"
#include "__tfm_internal.h"

#include "__trace_internal.h"

#include <errno.h>

static inline uint8_t hamming_weight(uint8_t n)
{
    return __builtin_popcount(n);
//    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
//    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
//    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);
//    return n;
}

static inline int pm_generic(uint8_t *data, int index, int div, float *res)
{
    int i;
    float sum = 0;

    for(i = 0; i < div; i++)
        sum += (float) hamming_weight(data[div * index + i]);

    *res = sum;
    return 0;
}

int pm_8bit(uint8_t *data, int index, float *res)
{
    *res = (float) hamming_weight(data[index]);
    return 0;
}

int pm_16bit(uint8_t *data, int index, float *res)
{
    return pm_generic(data, index, 2, res);
}

int pm_32bit(uint8_t *data, int index, float *res)
{
    return pm_generic(data, index, 4, res);
}

int pm_64bit(uint8_t *data, int index, float *res)
{
    return pm_generic(data, index, 8, res);
}

int pm_128bit(uint8_t *data, int index, float *res)
{
    return pm_generic(data, index, 16, res);
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
    ts->num_samples = ts->prev->num_samples;
    return 0;
}

int tfm_io_correlation_exit(struct trace_set *ts, void *arg)
{
    if(!ts || !arg)
    {
        err("Invalid trace set or init arg\n");
        return -EINVAL;
    }

    free(arg);
    return 0;
}

void tfm_io_correlation_progress_title(char *dst, int len, size_t index, int count)
{
    snprintf(dst, len, "CPA %li", index);
}

int tfm_io_correlation(struct tfm **tfm, int granularity, int num)
{
    int ret;
    struct tfm_io_correlation_arg *arg;
    int (*model)(uint8_t *, int, float *);

    struct cpa_args cpa_args = {
            .power_model = NULL,
            .num_models = 1,
            .consumer_init = tfm_io_correlation_init,
            .consumer_exit = tfm_io_correlation_exit,
            .progress_title = tfm_io_correlation_progress_title,
            .init_args = NULL
    };

    switch(granularity)
    {
        case 8:
            model = pm_8bit;
            break;

        case 16:
            model = pm_16bit;
            break;

        case 32:
            model = pm_32bit;
            break;

        case 64:
            model = pm_64bit;
            break;

        case 128:
            model = pm_128bit;
            break;

        default:
            err("Unsupported granularity\n");
            return -EINVAL;
    }

    arg = calloc(1, sizeof(struct tfm_io_correlation_arg));
    if(!arg)
    {
        err("Failed to allocate CPA arg\n");
        return -ENOMEM;
    }

    arg->granularity = granularity;
    arg->num = num;

    cpa_args.power_model = model;
    cpa_args.init_args = arg;

    ret = tfm_cpa(tfm, &cpa_args);
    if(ret < 0)
    {
        err("Failed to initialize generic CPA transform\n");
        free(arg);
        return ret;
    }

    return 0;
}