#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int stat_create_single_array(struct accumulator **acc, int num)
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

    res->acc_type = ACC_SINGLE_ARRAY;
    res->dim0 = num;
    res->dim1 = 0;
    res->acc_count = 0;

    res->acc_m.a = calloc(num, sizeof(float));
    if(!res->acc_m.a)
    {
        err("Failed to allocate single m array\n");
        goto __free_acc;
    }

    res->acc_s.a = calloc(num, sizeof(float));
    if(!res->acc_s.a)
    {
        err("Failed to allocate single s array\n");
        goto __free_acc;
    }

    res->acc_cov.f = 0;

    *acc = res;
    return 0;

__free_acc:
    if(res->acc_m.a)
        free(res->acc_m.a);

    if(res->acc_s.a)
        free(res->acc_s.a);

    free(res);
    return -ENOMEM;
}


int stat_accumulate_single_array(struct accumulator *acc, float *val, int len)
{
    int i;
    float m_new_scalar;

    IF_HAVE_512(__m512 curr_512, count_512, m_512, m_new_512, s_512, s_new_512);
    IF_HAVE_256(__m256 curr_256, count_256, m_256, m_new_256, s_256, s_new_256);
    IF_HAVE_128(__m128 curr_, count_, m_, m_new_, s_, s_new_);

    if(!acc || !val)
    {
        err("Invalid accumulator or data array\n");
        return -EINVAL;
    }

    if(acc->acc_type != ACC_SINGLE_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len)
    {
        err("Invalid data dimension\n");
        return -EINVAL;
    }

    acc->acc_count++;
    if(acc->acc_count == 1)
    {
        for(i = 0; i < len;)
        {
            LOOP_HAVE_512(i, len,
                          init_m(AVX512, &acc->acc_m.a[i], &val[i]));

            LOOP_HAVE_256(i, len,
                          init_m(AVX256, &acc->acc_m.a[i], &val[i]));

            LOOP_HAVE_128(i, len,
                          init_m(AVX128, &acc->acc_m.a[i], &val[i]))

            acc->acc_m.a[i] = val[i];
            acc->acc_s.a[i] = 0;
            i++;
        }
    }
    else
    {
        IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->acc_count));
        IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->acc_count));
        IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

        for(i = 0; i < len;)
        {
            LOOP_HAVE_512(i, len, accumulate(AVX512, m, s, curr, count,
                                             &acc->acc_m.a[i],
                                             &acc->acc_s.a[i],
                                             &val[i])
            );

            LOOP_HAVE_256(i, len, accumulate(AVX256, m, s, curr, count,
                                             &acc->acc_m.a[i],
                                             &acc->acc_s.a[i],
                                             &val[i])
            );

            LOOP_HAVE_128(i, len, accumulate(AVX128, m, s, curr, count,
                                             &acc->acc_m.a[i],
                                             &acc->acc_s.a[i],
                                             &val[i])
            );

            m_new_scalar = acc->acc_m.a[i] + (val[i] - acc->acc_m.a[i]) / acc->acc_count;
            acc->acc_s.a[i] += ((val[i] - acc->acc_m.a[i]) * (val[i] - m_new_scalar));
            acc->acc_m.a[i] = m_new_scalar;
            i++;
        }
    }

    return 0;
}

int __get_mean_single_array(struct accumulator *acc, int index, float *res)
{
    if(index >= acc->dim0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->acc_m.a[index];
    return 0;
}

int __get_dev_single_array(struct accumulator *acc, int index, float *res)
{
    if(index >= acc->dim0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = sqrtf(acc->acc_s.a[index] / (acc->acc_count - 1));
    return 0;
}

int __get_mean_single_array_all(struct accumulator *acc, float **res)
{
    float *result = calloc(acc->dim0, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    memcpy(result, acc->acc_m.a, acc->dim0 * sizeof(float));
    *res = result;
    return 0;
}

int __get_dev_single_array_all(struct accumulator *acc, float **res)
{
    int i;
    float count;

    IF_HAVE_512(__m512 count_512);
    IF_HAVE_256(__m256 count_256);
    IF_HAVE_128(__m128 count_);

    float *result = calloc(acc->dim0, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    count = acc->acc_count - 1;
    IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
    IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
    IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

    for(i = 0; i < acc->dim0;)
    {
        LOOP_HAVE_512(i, acc->dim0,
                      reduce_dev(AVX512, &acc->acc_s.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_256(i, acc->dim0,
                      reduce_dev(AVX256, &acc->acc_s.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_128(i, acc->dim0,
                      reduce_dev(AVX128, &acc->acc_s.a[i],
                                 &result[i], count);
        );

        result[i] = sqrtf(acc->acc_s.a[i] / count);
        i++;
    }

    *res = result;
    return 0;
}