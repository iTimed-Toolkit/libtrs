#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"
#include "__avx_macros.h"
#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


int __stat_reset_single_array(struct accumulator *acc)
{
    acc->count = 0;
    CAP_RESET_ARRAY(acc, _AVG, 0, acc->dim0);
    CAP_RESET_ARRAY(acc, _DEV, 0, acc->dim0);
    CAP_RESET_ARRAY(acc, _MAX, 0, acc->dim0);
    CAP_RESET_ARRAY(acc, _MIN, 0, acc->dim0);
    CAP_RESET_ARRAY(acc, _MAXABS, 0, acc->dim0);
    CAP_RESET_ARRAY(acc, _MINABS, 0, acc->dim0);
    return 0;
}

int __stat_free_single_array(struct accumulator *acc)
{
    CAP_FREE_ARRAY(acc, _AVG);
    CAP_FREE_ARRAY(acc, _DEV);
    CAP_FREE_ARRAY(acc, _MAX);
    CAP_FREE_ARRAY(acc, _MIN);
    CAP_FREE_ARRAY(acc, _MAXABS);
    CAP_FREE_ARRAY(acc, _MINABS);
    return 0;
}

int __stat_get_single_array(struct accumulator *acc, stat_t stat, int index, float *res)
{
    float val;
    IF_NOT_CAP(acc, stat)
    {
        err("Accumulator does not have requested capability\n");
        return -EINVAL;
    }

    if(index >= acc->dim0)
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    switch(stat)
    {
        case STAT_AVG:
            val = acc->_AVG.a[index]; break;

        case STAT_DEV:
            val = sqrtf(acc->_DEV.a[index] / (acc->count - 1)); break;

        case STAT_MAX:
            val = acc->_MAX.a[index]; break;

        case STAT_MIN:
            val = acc->_MIN.a[index]; break;

        case STAT_MAXABS:
            val = acc->_MAXABS.a[index]; break;

        case STAT_MINABS:
            val = acc->_MINABS.a[index]; break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    *res = val;
    return 0;
}

int __stat_get_all_single_array(struct accumulator *acc, stat_t stat, float **res)
{
    int i;
    float *result, count;

    IF_HAVE_512(__m512 count_512);
    IF_HAVE_256(__m256 count_256);
    IF_HAVE_128(__m128 count_);

    IF_NOT_CAP(acc, stat)
    {
        err("Accumulator does not have requested capability\n");
        return -EINVAL;
    }

    result = calloc(acc->dim0, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    switch(stat)
    {
        case STAT_AVG:
            memcpy(result, acc->_AVG.a, acc->dim0 * sizeof(float));
            break;

        case STAT_DEV:
            count = acc->count - 1;
            IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
            IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
            IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

            for(i = 0; i < acc->dim0;)
            {
                LOOP_HAVE_512(i, acc->dim0,
                              reduce_dev(AVX512, &acc->_DEV.a[i],
                                         &result[i], count);
                );

                LOOP_HAVE_256(i, acc->dim0,
                              reduce_dev(AVX256, &acc->_DEV.a[i],
                                         &result[i], count);
                );

                LOOP_HAVE_128(i, acc->dim0,
                              reduce_dev(AVX128, &acc->_DEV.a[i],
                                         &result[i], count);
                );

                result[i] = sqrtf(acc->_DEV.a[i] / count);
                i++;
            }
            break;

        case STAT_MAX:
            memcpy(result, acc->_MAX.a, acc->dim0 * sizeof(float));
            break;

        case STAT_MIN:
            memcpy(result, acc->_MIN.a, acc->dim0 * sizeof(float));
            break;

        case STAT_MAXABS:
            memcpy(result, acc->_MAXABS.a, acc->dim0 * sizeof(float));
            break;

        case STAT_MINABS:
            memcpy(result, acc->_MINABS.a, acc->dim0 * sizeof(float));
            break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    *res = result;
    return 0;
}

int stat_create_single_array(struct accumulator **acc, stat_t capabilities, int num)
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

    res->type = ACC_SINGLE_ARRAY;
    res->dim0 = num;
    res->dim1 = 0;
    res->count = 0;

    CAP_INIT_ARRAY(res, _AVG, num, __free_acc);
    CAP_INIT_ARRAY(res, _DEV, num, __free_acc);
    CAP_INIT_ARRAY(res, _MAX, num, __free_acc);
    CAP_INIT_ARRAY(res, _MIN, num, __free_acc);
    CAP_INIT_ARRAY(res, _MAXABS, num, __free_acc);
    CAP_INIT_ARRAY(res, _MINABS, num, __free_acc);

    res->reset = __stat_reset_single_array;
    res->free = __stat_free_single_array;
    res->get = __stat_get_single_array;
    res->get_all = __stat_get_all_single_array;

    *acc = res;
    return 0;

__free_acc:
    CAP_FREE_ARRAY(res, _AVG);
    CAP_FREE_ARRAY(res, _DEV);
    CAP_FREE_ARRAY(res, _MAX);
    CAP_FREE_ARRAY(res, _MIN);
    CAP_FREE_ARRAY(res, _MAXABS);
    CAP_FREE_ARRAY(res, _MINABS);
    free(res);
    return -ENOMEM;
}

#if defined(LIBTRACE_PLATFORM_LINUX)
__attribute__ ((always_inline)) static inline
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
static __forceinline
#endif
int __accumulate_single_array(struct accumulator *acc, float *val, int len)
{
    int i;
    float m_new_scalar;

    IF_HAVE_512(__m512 curr_512, count_512, m_512, m_new_512, s_512, s_new_512, bound_512);
    IF_HAVE_256(__m256 curr_256, count_256, m_256, m_new_256, s_256, s_new_256, bound_256);
    IF_HAVE_128(__m128 curr_, count_, m_, m_new_, s_, s_new_, bound_);

    acc->count++;
    if(acc->count == 1)
    {
        IF_CAP(acc, _AVG) memcpy(acc->_AVG.a, val, len * sizeof(float));
        IF_CAP(acc, _DEV) memset(acc->_DEV.a, 0, len * sizeof(float));
        IF_CAP(acc, _MAX) memcpy(acc->_MAX.a, val, len * sizeof(float));
        IF_CAP(acc, _MIN) memcpy(acc->_MIN.a, val, len * sizeof(float));

        for(i = 0; i < len;)
        {
            LOOP_HAVE_512(i, len,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX512, &acc->_MAXABS.a[i], &val[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX512, &acc->_MINABS.a[i], &val[i]); });

            LOOP_HAVE_256(i, len,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX256, &acc->_MAXABS.a[i], &val[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX256, &acc->_MINABS.a[i], &val[i]); });

            LOOP_HAVE_128(i, len,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX128, &acc->_MAXABS.a[i], &val[i]) }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX128, &acc->_MINABS.a[i], &val[i]) });

            IF_CAP(acc, _MAXABS) acc->_MAXABS.a[i] = fabsf(val[i]);
            IF_CAP(acc, _MINABS) acc->_MINABS.a[i] = fabsf(val[i]);
            i++;
        }
    }
    else
    {
        IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->count));
        IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->count));
        IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

        for(i = 0; i < len;)
        {
            LOOP_HAVE_512(i, len,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX512, m, s, curr, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val[i]);
                          }
                          IF_CAP(acc, _MAX) {
                              accumulate_max(AVX512, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MIN) {
                              accumulate_min(AVX512, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX512, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                          IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX512, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            )

            LOOP_HAVE_256(i, len,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX256, m, s, curr, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val[i]);
                          }
                          IF_CAP(acc, _MAX) {
                              accumulate_max(AVX256, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MIN) {
                              accumulate_min(AVX256, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX256, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                          IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX256, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            );

            LOOP_HAVE_128(i, len,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX128, m, s, curr, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val[i]);
                          }
                          IF_CAP(acc, _MAX) {
                              accumulate_max(AVX128, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MIN) {
                              accumulate_min(AVX128, curr, &val[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                          IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX128, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                          IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX128, curr, &val[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            );

            IF_CAP(acc, _AVG)
            {
                m_new_scalar = acc->_AVG.a[i] + (val[i] - acc->_AVG.a[i]) / acc->count;
                acc->_DEV.a[i] += ((val[i] - acc->_AVG.a[i]) * (val[i] - m_new_scalar));
                acc->_AVG.a[i] = m_new_scalar;
            }

            IF_CAP(acc, _MAX)
                acc->_MAX.a[i] = (val[i] > acc->_MAX.a[i] ? val[i] : acc->_MAX.a[i]);

            IF_CAP(acc, _MIN)
                acc->_MIN.a[i] = (val[i] < acc->_MIN.a[i] ? val[i] : acc->_MIN.a[i]);

            IF_CAP(acc, _MAXABS)
                acc->_MAXABS.a[i] = (fabsf(val[i]) > acc->_MAXABS.a[i] ?
                                     fabsf(val[i]) : acc->_MAXABS.a[i]);
            IF_CAP(acc, _MINABS)
                acc->_MINABS.a[i] = (fabsf(val[i]) < acc->_MINABS.a[i] ?
                                     fabsf(val[i]) : acc->_MINABS.a[i]);
            i++;
        }
    }

    return 0;
}

int stat_accumulate_single_array(struct accumulator *acc, float *val, int len)
{
    if(!acc || !val)
    {
        err("Invalid accumulator or data array\n");
        return -EINVAL;
    }

    if(acc->type != ACC_SINGLE_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len)
    {
        err("Invalid data dimension\n");
        return -EINVAL;
    }

    return __accumulate_single_array(acc, val, len);
}

int stat_accumulate_single_array_many(struct accumulator *acc, float *val, int len, int num)
{
    int i, ret;

    if(!acc || !val)
    {
        err("Invalid accumulator or data array\n");
        return -EINVAL;
    }

    if(acc->type != ACC_SINGLE_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len)
    {
        err("Invalid data dimension\n");
        return -EINVAL;
    }

    for(i = 0; i < num; i++)
    {
        ret = __accumulate_single_array(acc, &val[len * i], len);
        if(ret < 0)
        {
            err("Failed to accumulate at index %i\n", i);
            return ret;
        }
    }

    return 0;
}