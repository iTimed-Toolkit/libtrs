#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"
#include "__avx_macros.h"
#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AUTO_TRANSPOSE      1

int __stat_reset_dual_array(struct accumulator *acc)
{
    acc->count = 0;
    CAP_RESET_ARRAY(acc, _AVG, 0, acc->dim0 + acc->dim1);
    CAP_RESET_ARRAY(acc, _DEV, 0, acc->dim0 + acc->dim1);
    CAP_RESET_ARRAY(acc, _COV, 0, acc->dim0 * acc->dim1);
    CAP_RESET_ARRAY(acc, _MAX, 0, acc->dim0 + acc->dim1);
    CAP_RESET_ARRAY(acc, _MIN, 0, acc->dim0 + acc->dim1);
    CAP_RESET_ARRAY(acc, _MAXABS, 0, acc->dim0 + acc->dim1);
    CAP_RESET_ARRAY(acc, _MINABS, 0, acc->dim0 + acc->dim1);
    return 0;
}

int __stat_free_dual_array(struct accumulator *acc)
{
    CAP_FREE_ARRAY(acc, _AVG);
    CAP_FREE_ARRAY(acc, _DEV);
    CAP_FREE_ARRAY(acc, _COV);
    CAP_FREE_ARRAY(acc, _MAX);
    CAP_FREE_ARRAY(acc, _MIN);
    CAP_FREE_ARRAY(acc, _MAXABS);
    CAP_FREE_ARRAY(acc, _MINABS);

#if USE_GPU
    gpu_free_dual_array(acc);
#endif
    return 0;
}

/*
 * In array is len0 x len1, out array is len1 x len0.
 * Note that this could be more efficient, but this call
 * is not usually performance critical
 */
void __transpose(int len0, int len1, float *in, float *out)
{
    int y, x, i1, i2;
    for(y = 0; y < len0; y++)
    {
        for(x = 0; x < len1; x++)
        {
            i1 = y * len1 + x;
            i2 = x * len0 + y;
            out[i2] = in[i1];
        }
    }
}

int __stat_get_dual_array(struct accumulator *acc, stat_t stat, int index, float *res)
{
    int i0, i1;
    float val;
    IF_NOT_CAP(acc, stat)
    {
        err("Accumulator does not have requested capability\n");
        return -EINVAL;
    }

    if(stat & (STAT_COV | STAT_PEARSON) && index >= acc->dim0 * acc->dim1)
    {
        err("Invalid index for accumulator and statistic\n");
        return -EINVAL;
    }
    else if(index >= acc->dim0 + acc->dim1)
    {
        err("Invalid index for accumulator and statistic\n");
        return -EINVAL;
    }

#if USE_GPU
    int ret = gpu_sync_dual_array(acc);
    if(ret < 0)
    {
        err("Failed to sync values from GPU\n");
        return ret;
    }
#endif

    if(acc->transpose)
    {
        switch(stat)
        {
            case STAT_AVG:
            case STAT_DEV:
            case STAT_MAX:
            case STAT_MIN:
            case STAT_MAXABS:
            case STAT_MINABS:
                index = (acc->dim0 + acc->dim1 - index);
                break;

            case STAT_COV:
            case STAT_PEARSON:
                i0 = (index % acc->dim0);
                i1 = (index / acc->dim0);
                index = i0 * acc->dim0 + i1;
                break;

            default:
                err("Invalid requested statistic\n");
                return -EINVAL;
        }
    }

    switch(stat)
    {
        case STAT_AVG:
            val = acc->_AVG.a[index];
            break;

        case STAT_DEV:
            val = sqrtf(acc->_DEV.a[index] / (acc->count - 1));
            break;

        case STAT_COV:
            val = acc->_COV.a[index];
            break;

        case STAT_PEARSON:
            i0 = (index % acc->dim0);
            i1 = (index / acc->dim0);

            val = acc->_COV.a[index] /
                  ((acc->count - 1) *
                   sqrtf(acc->_DEV.a[i0] / (acc->count - 1)) *
                   sqrtf(acc->_DEV.a[acc->dim0 + i1] / (acc->count - 1)));
            break;

        case STAT_MAX:
            val = acc->_MAX.a[index];
            break;

        case STAT_MIN:
            val = acc->_MIN.a[index];
            break;

        case STAT_MAXABS:
            val = acc->_MAXABS.a[index];
            break;

        case STAT_MINABS:
            val = acc->_MINABS.a[index];
            break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    *res = val;
    return 0;
}

int __stat_get_all_dual_array(struct accumulator *acc, stat_t stat, float **res)
{
    int i, j, len;
    float *result, *temp,
            *source, *source_dev,
            count, dev;

    IF_HAVE_512(__m512 count_512, dev_512);
    IF_HAVE_256(__m256 count_256, dev_256);
    IF_HAVE_128(__m128 count_, dev_);

#if USE_GPU
    int ret = gpu_sync_dual_array(acc);
    if(ret < 0)
    {
        err("Failed to sync values from GPU\n");
        return ret;
    }
#endif

    if(stat & (STAT_COV | STAT_PEARSON))
        result = calloc(acc->dim0 * acc->dim1, sizeof(float));
    else
        result = calloc(acc->dim0 + acc->dim1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    if(acc->transpose)
    {
        if(stat & (STAT_COV | STAT_PEARSON))
            temp = calloc(acc->dim0 * acc->dim1, sizeof(float));
        else
            temp = calloc(acc->dim0 + acc->dim1, sizeof(float));
        if(!temp)
        {
            err("Failed to allocate result\n");
            return -ENOMEM;
        }
    }

    switch(stat)
    {
        case STAT_AVG:
            if(acc->transpose)
            {
                memcpy(&result[0], &acc->_AVG.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&result[acc->dim0], &acc->_AVG.a[0], acc->dim1 * sizeof(float));
            }
            else memcpy(result, acc->_AVG.a, (acc->dim0 + acc->dim1) * sizeof(float));
            break;

        case STAT_DEV:
            count = acc->count - 1;
            IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
            IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
            IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

            if(acc->transpose)
            {
                memcpy(&temp[0], &acc->_DEV.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&temp[acc->dim0], &acc->_DEV.a[0], acc->dim1 * sizeof(float));
                source = temp;
            }
            else source = acc->_DEV.a;

            for(i = 0; i < acc->dim0 + acc->dim1;)
            {
                LOOP_HAVE_512(i, acc->dim0 + acc->dim1,
                              reduce_dev(AVX512, &source[i],
                                         &result[i], count);
                );

                LOOP_HAVE_256(i, acc->dim0 + acc->dim1,
                              reduce_dev(AVX256, &source[i],
                                         &result[i], count);
                );

                LOOP_HAVE_128(i, acc->dim0 + acc->dim1,
                              reduce_dev(AVX128, &source[i],
                                         &result[i], count);
                );

                result[i] = sqrtf(source[i] / count);
                i++;
            }
            break;

        case STAT_COV:
            len = acc->dim0 * acc->dim1;
            count = acc->count - 1;
            IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->count));
            IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->count));
            IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

            if(acc->transpose)
            {
                __transpose(acc->dim0, acc->dim1, acc->_COV.a, temp);
                source = temp;
            }
            else source = acc->_COV.a;

            for(i = 0; i < len;)
            {
                LOOP_HAVE_512(i, len,
                              reduce_cov(AVX512, &source[i],
                                         &result[i], count);
                );

                LOOP_HAVE_256(i, len,
                              reduce_cov(AVX256, &source[i],
                                         &result[i], count);
                );

                LOOP_HAVE_128(i, len,
                              reduce_cov(AVX128, &source[i],
                                         &result[i], count);
                );

                result[i] = source[i] / acc->count;
                i++;
            }
            break;

        case STAT_PEARSON:
            count = acc->count - 1;
            IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
            IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
            IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

            if(acc->transpose)
            {
                __transpose(acc->dim0, acc->dim1, acc->_COV.a, temp);
                source = temp;

                source_dev = calloc(acc->dim0 + acc->dim1, sizeof(float));
                if(!source_dev)
                {
                    err("Failed to allocate temp array to hold deviation\n");
                    free(temp); return -ENOMEM;
                }

                memcpy(&source_dev[0], &acc->_DEV.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&source_dev[acc->dim0], &acc->_DEV.a[0], acc->dim1 * sizeof(float));
            }
            else
            {
                source = acc->_COV.a;
                source_dev = acc->_DEV.a;
            }

            for(j = 0; j < acc->dim1; j++)
            {
                dev = sqrtf(source_dev[acc->dim0 + j] / count);
                IF_HAVE_128(dev_ = _mm_broadcast_ss(&dev));
                IF_HAVE_256(dev_256 = _mm256_broadcast_ss(&dev));
                IF_HAVE_512(dev_512 = _mm512_broadcastss_ps(dev_));

                for(i = 0; i < acc->dim0;)
                {
                    LOOP_HAVE_512(i, acc->dim0,
                                  reduce_pearson(AVX512, &source[j * acc->dim0 + i],
                                                 &source_dev[i], dev,
                                                 &result[j * acc->dim0 + i], count);
                    );

                    LOOP_HAVE_256(i, acc->dim0,
                                  reduce_pearson(AVX256, &source[j * acc->dim0 + i],
                                                 &source_dev[i], dev,
                                                 &result[j * acc->dim0 + i], count);
                    );

                    LOOP_HAVE_128(i, acc->dim0,
                                  reduce_pearson(AVX128, &source[j * acc->dim0 + i],
                                                 &source_dev[i], dev,
                                                 &result[j * acc->dim0 + i], count);
                    );

                    result[j * acc->dim0 + i] = source[j * acc->dim0 + i] /
                                                (count * dev *
                                                 sqrtf(source_dev[i] / count));
                    i++;
                }
            }

            if(acc->transpose)
                free(source_dev);
            break;

        case STAT_MAX:
            if(acc->transpose)
            {
                memcpy(&result[0], &acc->_MAX.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&result[acc->dim0], &acc->_MAX.a[0], acc->dim1 * sizeof(float));
            }
            else memcpy(result, acc->_MAX.a, (acc->dim0 + acc->dim1) * sizeof(float));
            break;

        case STAT_MIN:
            if(acc->transpose)
            {
                memcpy(&result[0], &acc->_MIN.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&result[acc->dim0], &acc->_MIN.a[0], acc->dim1 * sizeof(float));
            }
            else memcpy(result, acc->_MIN.a, (acc->dim0 + acc->dim1) * sizeof(float));
            break;

        case STAT_MAXABS:
            if(acc->transpose)
            {
                memcpy(&result[0], &acc->_MAXABS.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&result[acc->dim0], &acc->_MAXABS.a[0], acc->dim1 * sizeof(float));
            }
            else memcpy(result, acc->_MAXABS.a, (acc->dim0 + acc->dim1) * sizeof(float));
            break;

        case STAT_MINABS:
            if(acc->transpose)
            {
                memcpy(&result[0], &acc->_MINABS.a[acc->dim1], acc->dim0 * sizeof(float));
                memcpy(&result[acc->dim0], &acc->_MINABS.a[0], acc->dim1 * sizeof(float));
            }
            else memcpy(result, acc->_MINABS.a, (acc->dim0 + acc->dim1) * sizeof(float));
            break;

        default:
            err("Invalid requested statistic\n");
            return -EINVAL;
    }

    if(temp)
        free(temp);

    *res = result;
    return 0;
}

int stat_create_dual_array(struct accumulator **acc, stat_t capabilities, int num0, int num1)
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

    res->type = ACC_DUAL_ARRAY;
    res->capabilities = capabilities;
    res->dim0 = num0;
    res->dim1 = num1;
    res->count = 0;
    res->transpose = false;

#if AUTO_TRANSPOSE
    if(num0 < num1)
    {
        warn("Dual accumulators should have smaller second dimension (for efficiency). "
             "Automatically transposing your data.\n");
        res->transpose = true;
    }
#endif

    CAP_INIT_ARRAY(res, _AVG, num0 + num1, __free_acc);
    CAP_INIT_ARRAY(res, _DEV, num0 + num1, __free_acc);
    CAP_INIT_ARRAY(res, _COV, num0 * num1, __free_acc);
    CAP_INIT_ARRAY(res, _MAX, num0 + num1, __free_acc);
    CAP_INIT_ARRAY(res, _MIN, num0 + num1, __free_acc);
    CAP_INIT_ARRAY(res, _MAXABS, num0 + num1, __free_acc);
    CAP_INIT_ARRAY(res, _MINABS, num0 + num1, __free_acc);

    res->reset = __stat_reset_dual_array;
    res->free = __stat_free_dual_array;
    res->get = __stat_get_dual_array;
    res->get_all = __stat_get_all_dual_array;

#if USE_GPU
    int ret;
    if(res->transpose)
    {
        ret = gpu_init_dual_array(res, num1, num0);
        if(ret < 0)
        {
            err("Failed to initialize GPU-side vars\n");
            goto __free_acc;
        }
    }
    else
    {
        ret = gpu_init_dual_array(res, num0, num1);
        if(ret < 0)
        {
            err("Failed to initialize GPU-side vars\n");
            goto __free_acc;
        }
    }
#endif

    *acc = res;
    return 0;

__free_acc:
    CAP_FREE_ARRAY(res, _AVG);
    CAP_FREE_ARRAY(res, _DEV);
    CAP_FREE_ARRAY(res, _COV);
    CAP_FREE_ARRAY(res, _MAX);
    CAP_FREE_ARRAY(res, _MIN);
    CAP_FREE_ARRAY(res, _MAXABS);
    CAP_FREE_ARRAY(res, _MINABS);
    free(res);
    return -ENOMEM;
}

#if defined(LIBTRACE_PLATFORM_LINUX)
__attribute__((always_inline)) static inline
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
static __forceinline
#endif
int __accumulate_dual_array(struct accumulator *acc,
                                          float *val0, float *val1,
                                          int len0, int len1)
{
#if USE_GPU
    return gpu_accumulate_dual_array(acc, val0, val1, len0, len1);
#else
    int  j, k;
    float m0_new_scalar, m1_new_scalar;

    IF_HAVE_512(__m512 curr0_512, curr1_512, count_512,
                m0_512, m0_new_512, s0_512, s0_new_512,
                m1_512, m1_new_512, s1_512, s1_new_512,
                cov_512, cov_new_512, cov_curr_512, cov_m_512, bound_512);
    IF_HAVE_256(__m256 curr0_256, curr1_256, count_256,
                m0_256, m0_new_256, s0_256, s0_new_256,
                m1_256, m1_new_256, s1_256, s1_new_256,
                cov_256, cov_new_256, cov_curr_256, cov_m_256, bound_256);
    IF_HAVE_128(__m128 curr0_, curr1_, count_,
                m0_, m0_new_, s0_, s0_new_,
                m1_, m1_new_, s1_, s1_new_,
                cov_, cov_new_, cov_curr_, cov_m_, bound_);

    acc->count++;
    if(acc->count == 1)
    {
        IF_CAP(acc, _AVG)
        {
            memcpy(&acc->_AVG.a[0], val0, len0 * sizeof(float));
            memcpy(&acc->_AVG.a[len0], val1, len1 * sizeof(float));
        }

        IF_CAP(acc, _DEV) memset(acc->_DEV.a, 0, (len0 + len1) * sizeof(float));

        IF_CAP(acc, _MAX)
        {
            memcpy(&acc->_MAX.a[0], val0, len0 * sizeof(float));
            memcpy(&acc->_MAX.a[len0], val1, len1 * sizeof(float));
        }

        IF_CAP(acc, _MIN)
        {
            memcpy(&acc->_MIN.a[0], val0, len0 * sizeof(float));
            memcpy(&acc->_MIN.a[len0], val1, len1 * sizeof(float));
        }

        for(i = 0; i < len0;)
        {
            LOOP_HAVE_512(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX512, &acc->_MAXABS.a[i], &val0[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX512, &acc->_MINABS.a[i], &val0[i]); });

            LOOP_HAVE_256(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX256, &acc->_MAXABS.a[i], &val0[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX256, &acc->_MINABS.a[i], &val0[i]); });

            LOOP_HAVE_128(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX128, &acc->_MAXABS.a[i], &val0[i]) }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX128, &acc->_MINABS.a[i], &val0[i]) });

            IF_CAP(acc, _MAXABS) acc->_MAXABS.a[i] = fabsf(val0[i]);
            IF_CAP(acc, _MINABS) acc->_MINABS.a[i] = fabsf(val0[i]);
            i++;
        }

        for(i = 0; i < len1;)
        {
            LOOP_HAVE_512(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX512, &acc->_MAXABS.a[len0 + i], &val1[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX512, &acc->_MINABS.a[len0 + i], &val1[i]); });

            LOOP_HAVE_256(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX256, &acc->_MAXABS.a[len0 + i], &val1[i]); }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX256, &acc->_MINABS.a[len0 + i], &val1[i]); });

            LOOP_HAVE_128(i, len0,
                          IF_CAP(acc, _MAXABS) { init_abs(AVX128, &acc->_MAXABS.a[len0 + i], &val1[i]) }
                                  IF_CAP(acc, _MINABS) { init_abs(AVX128, &acc->_MINABS.a[len0 + i], &val1[i]) });

            IF_CAP(acc, _MAXABS) acc->_MAXABS.a[len0 + i] = fabsf(val1[i]);
            IF_CAP(acc, _MINABS) acc->_MINABS.a[len0 + i] = fabsf(val1[i]);
            i++;
        }
    }
    else
    {
        IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->count));
        IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->count));
        IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

        for(i = 0; i < len0;)
        {
            LOOP_HAVE_512(i, len0,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX512, m0, s0, curr0, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val0[i]);
                          }
                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX512, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX512, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX512, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX512, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            )

            LOOP_HAVE_256(i, len0,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX256, m0, s0, curr0, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val0[i]);
                          }
                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX256, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX256, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX256, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX256, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            );

            LOOP_HAVE_128(i, len0,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX128, m0, s0, curr0, count,
                                         &acc->_AVG.a[i],
                                         &acc->_DEV.a[i],
                                         &val0[i]);
                          }
                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX128, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX128, curr0, &val0[i],
                                             bound, &acc->_MAX.a[i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX128, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX128, curr0, &val0[i],
                                                bound, &acc->_MAXABS.a[i]);
                          }
            );

            IF_CAP(acc, _AVG)
            {
                m0_new_scalar = acc->_AVG.a[i] + (val0[i] - acc->_AVG.a[i]) / acc->count;
                acc->_DEV.a[i] += ((val0[i] - acc->_AVG.a[i]) * (val0[i] - m0_new_scalar));
                acc->_AVG.a[i] = m0_new_scalar;
            }

            IF_CAP(acc, _MAX)acc->_MAX.a[i] = (val0[i] > acc->_MAX.a[i] ? val0[i] : acc->_MAX.a[i]);

            IF_CAP(acc, _MIN)acc->_MIN.a[i] = (val0[i] < acc->_MIN.a[i] ? val0[i] : acc->_MIN.a[i]);

            IF_CAP(acc, _MAXABS)acc->_MAXABS.a[i] = (fabsf(val0[i]) > acc->_MAXABS.a[i] ?
                                                     fabsf(val0[i]) : acc->_MAXABS.a[i]);
            IF_CAP(acc, _MINABS)acc->_MINABS.a[i] = (fabsf(val0[i]) < acc->_MINABS.a[i] ?
                                                     fabsf(val0[i]) : acc->_MINABS.a[i]);
            i++;
        }

        for(i = 0; i < len1;)
        {
            LOOP_HAVE_512(i, len1,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX512, m1, s1, curr1, count,
                                         &acc->_AVG.a[len0 + i],
                                         &acc->_DEV.a[len0 + i],
                                         &val1[i]);
                          }
                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX512, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX512, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX512, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX512, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }

                                  IF_CAP(acc, _COV) {
                              for(j = 0; j < 16; j++)
                              {
                                  m1_new_scalar = mm512_extract(m1_512, j);
                                  IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                  IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                  for(k = 0; k < len0;)
                                  {
                                      LOOP_HAVE_512(k, len0,
                                                    accumulate_cov(AVX512, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      LOOP_HAVE_256(k, len0,
                                                    accumulate_cov(AVX256, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      LOOP_HAVE_128(k, len0,
                                                    accumulate_cov(AVX128, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      acc->_COV.a[len0 * (i + j) + k] +=
                                              ((val1[i + j] - m1_new_scalar) *
                                               (val0[k] - acc->_AVG.a[k]));
                                      k++;
                                  }
                              }
                          }
            );

            LOOP_HAVE_256(i, len1,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX256, m1, s1, curr1, count,
                                         &acc->_AVG.a[len0 + i],
                                         &acc->_DEV.a[len0 + i],
                                         &val1[i]);
                          }

                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX256, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX256, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX256, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX256, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }

                                  IF_CAP(acc, _COV) {
                              for(j = 0; j < 8; j++)
                              {
                                  m1_new_scalar = mm256_extract(m1_256, j);
                                  IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                  IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                  for(k = 0; k < len0;)
                                  {
                                      LOOP_HAVE_512(k, len0,
                                                    accumulate_cov(AVX512, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      LOOP_HAVE_256(k, len0,
                                                    accumulate_cov(AVX256, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k])
                                      );

                                      LOOP_HAVE_128(k, len0,
                                                    accumulate_cov(AVX128, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      acc->_COV.a[len0 * (i + j) + k] +=
                                              ((val1[i + j] - m1_new_scalar) *
                                               (val0[k] - acc->_AVG.a[k]));
                                      k++;
                                  }
                              }
                          }
            );

            LOOP_HAVE_128(i, len1,
                          IF_CAP(acc, _AVG) {
                              accumulate(AVX128, m1, s1, curr1, count,
                                         &acc->_AVG.a[len0 + i],
                                         &acc->_DEV.a[len0 + i],
                                         &val1[i]);
                          }

                                  IF_CAP(acc, _MAX) {
                              accumulate_max(AVX128, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MIN) {
                              accumulate_min(AVX128, curr1, &val1[i],
                                             bound, &acc->_MAX.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MAXABS) {
                              accumulate_maxabs(AVX128, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }
                                  IF_CAP(acc, _MINABS) {
                              accumulate_minabs(AVX128, curr1, &val1[i],
                                                bound, &acc->_MAXABS.a[len0 + i]);
                          }

                                  IF_CAP(acc, _COV) {
                              for(j = 0; j < 4; j++)
                              {
                                  m1_new_scalar = mm128_extract(m1_, j);
                                  IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                  IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                  IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                  IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                  for(k = 0; k < len0;)
                                  {
                                      LOOP_HAVE_512(k, len0,
                                                    accumulate_cov(AVX512, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      LOOP_HAVE_256(k, len0,
                                                    accumulate_cov(AVX256, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      LOOP_HAVE_128(k, len0,
                                                    accumulate_cov(AVX128, cov, &acc->_COV.a[len0 * (i + j) + k],
                                                                   cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                                      );

                                      acc->_COV.a[len0 * (i + j) + k] +=
                                              ((val1[i + j] - m1_new_scalar) *
                                               (val0[k] - acc->_AVG.a[k]));
                                      k++;
                                  }
                              }
                          }
            );

            IF_CAP(acc, _AVG)
            {
                m1_new_scalar = acc->_AVG.a[len0 + i] + (val1[i] - acc->_AVG.a[len0 + i]) / acc->count;
                acc->_DEV.a[len0 + i] += ((val1[i] - acc->_AVG.a[len0 + i]) * (val1[i] - m1_new_scalar));

                IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&acc->_AVG.a[len0 + i]));
                IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&acc->_AVG.a[len0 + i]));
                IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                acc->_AVG.a[len0 + i] = m1_new_scalar;
            }

            IF_CAP(acc, _MAX)acc->_MAX.a[len0 + i] = (val1[i] > acc->_MAX.a[len0 + i] ?
                                                      val1[i] : acc->_MAX.a[len0 + i]);

            IF_CAP(acc, _MIN)acc->_MIN.a[len0 + i] = (val1[i] < acc->_MIN.a[len0 + i] ?
                                                      val1[i] : acc->_MIN.a[len0 + i]);

            IF_CAP(acc, _MAXABS)acc->_MAXABS.a[len0 + i] = (fabsf(val1[i]) > acc->_MAXABS.a[len0 + i] ?
                                                            fabsf(val1[i]) : acc->_MAXABS.a[len0 + i]);

            IF_CAP(acc, _MINABS)acc->_MINABS.a[len0 + i] = (fabsf(val1[i]) < acc->_MINABS.a[len0 + i] ?
                                                            fabsf(val1[i]) : acc->_MINABS.a[len0 + i]);

            IF_CAP(acc, _COV)
            {
                IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i]));
                IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i]));
                IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                for(k = 0; k < len0;)
                {
                    LOOP_HAVE_512(k, len0,
                                  accumulate_cov(AVX512, cov, &acc->_COV.a[len0 * i + k],
                                                 cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                    );

                    LOOP_HAVE_256(k, len0,
                                  accumulate_cov(AVX256, cov, &acc->_COV.a[len0 * i + k],
                                                 cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                    );

                    LOOP_HAVE_128(k, len0,
                                  accumulate_cov(AVX128, cov, &acc->_COV.a[len0 * i + k],
                                                 cov_curr, cov_m, &val0[k], &acc->_AVG.a[k]);
                    );

                    acc->_COV.a[len0 * i + k] += ((val1[i] - m1_new_scalar) *
                                                  (val0[k] - acc->_AVG.a[k]));
                    k++;
                }
            }

            i++;
        }
    }

    return 0;
#endif
}

int stat_accumulate_dual_array(struct accumulator *acc, float *val0, float *val1, int len0, int len1)
{
    if(!acc || !val0 || !val1)
    {
        err("Invalid accumulator or data arrays\n");
        return -EINVAL;
    }

    if(acc->type != ACC_DUAL_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len0 || acc->dim1 != len1)
    {
        err("Invalid data dimensions\n");
        return -EINVAL;
    }

    if(acc->transpose)
        return __accumulate_dual_array(acc, val1, val0, len1, len0);
    else
        return __accumulate_dual_array(acc, val0, val1, len0, len1);
}

int stat_accumulate_dual_array_many(struct accumulator *acc, float *val0, float *val1, int len0, int len1, int num)
{
    int i, ret;
    if(!acc || !val0 || !val1)
    {
        err("Invalid accumulator or data arrays\n");
        return -EINVAL;
    }

    if(acc->type != ACC_DUAL_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len0 || acc->dim1 != len1)
    {
        err("Invalid data dimensions\n");
        return -EINVAL;
    }

    for(i = 0; i < num; i++)
    {
        ret = __accumulate_dual_array(acc, &val0[i * len0], &val1[i * len1], len0, len1);
        if(ret < 0)
        {
            err("Failed to accumulate at index %i\n", i);
            return ret;
        }
    }

    return 0;
}
