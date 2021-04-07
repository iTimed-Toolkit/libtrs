#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int stat_create_dual_array(struct accumulator **acc, int num0, int num1)
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
    res->dim0 = num0;
    res->dim1 = num1;
    res->count = 0;

    res->m.a = calloc(num0 + num1, sizeof(float));
    if(!res->m.a)
    {
        err("Failed to allocate dual m array\n");
        goto __free_acc;
    }

    res->s.a = calloc(num0 + num1, sizeof(float));
    if(!res->s.a)
    {
        err("Failed to allocate dual s array\n");
        goto __free_acc;
    }

    res->cov.a = calloc(num0 * num1, sizeof(float));
    if(!res->cov.a)
    {
        err("Failed to allocate dual cov array\n");
        goto __free_acc;
    }

    *acc = res;
    return 0;

__free_acc:
    if(res->m.a)
        free(res->m.a);

    if(res->s.a)
        free(res->s.a);

    if(res->cov.a)
        free(res->cov.a);

    free(res);
    return -ENOMEM;
}

int __accumulate_dual_array(struct accumulator *acc, float *val0, float *val1, int len0, int len1)
{
    int i, j, k;
    float m0_new_scalar, m1_new_scalar;

    IF_HAVE_512(__m512 curr0_512, curr1_512, count_512,
                m0_512, m0_new_512, s0_512, s0_new_512,
                m1_512, m1_new_512, s1_512, s1_new_512,
                cov_512, cov_new_512, cov_curr_512, cov_m_512);
    IF_HAVE_256(__m256 curr0_256, curr1_256, count_256,
                m0_256, m0_new_256, s0_256, s0_new_256,
                m1_256, m1_new_256, s1_256, s1_new_256,
                cov_256, cov_new_256, cov_curr_256, cov_m_256);
    IF_HAVE_128(__m128 curr0_, curr1_, count_,
                m0_, m0_new_, s0_, s0_new_,
                m1_, m1_new_, s1_, s1_new_,
                cov_, cov_new_, cov_curr_, cov_m_);

    acc->count++;
    if(acc->count == 1)
    {
        for(i = 0; i < len0;)
        {
            LOOP_HAVE_512(i, len0,
                          init_m(AVX512, &acc->m.a[i], &val0[i])
            );

            LOOP_HAVE_256(i, len0,
                          init_m(AVX256, &acc->m.a[i], &val0[i])
            );

            LOOP_HAVE_128(i, len0,
                          init_m(AVX128, &acc->m.a[i], &val0[i])
            )

            acc->m.a[i] = val0[i];
            acc->s.a[i] = 0;
            i++;
        }

        for(i = 0; i < len1;)
        {
            LOOP_HAVE_512(i, len1,
                          init_m(AVX512, &acc->m.a[len0 + i], &val1[i])
            );

            LOOP_HAVE_256(i, len1,
                          init_m(AVX256, &acc->m.a[len0 + i], &val1[i])
            );

            LOOP_HAVE_128(i, len1,
                          init_m(AVX128, &acc->m.a[len0 + i], &val1[i])
            );

            acc->m.a[len0 + i] = val1[i];
            acc->s.a[i] = 0;
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
                          accumulate(AVX512, m0, s0, curr0, count,
                                     &acc->m.a[i],
                                     &acc->s.a[i],
                                     &val0[i]);
            );

            LOOP_HAVE_256(i, len0,
                          accumulate(AVX256, m0, s0, curr0, count,
                                     &acc->m.a[i],
                                     &acc->s.a[i],
                                     &val0[i]);
            );

            LOOP_HAVE_128(i, len0,
                          accumulate(AVX128, m0, s0, curr0, count,
                                     &acc->m.a[i],
                                     &acc->s.a[i],
                                     &val0[i]);
            );

            m0_new_scalar = acc->m.a[i] + (val0[i] - acc->m.a[i]) / acc->count;
            acc->s.a[i] += ((val0[i] - acc->m.a[i]) * (val0[i] - m0_new_scalar));
            acc->m.a[i] = m0_new_scalar;
            i++;
        }

        for(i = 0; i < len1;)
        {
            LOOP_HAVE_512(i, len1,
                          accumulate(AVX512, m1, s1, curr1, count,
                                     &acc->m.a[len0 + i],
                                     &acc->s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 16; j++)
                                  {
                                      m1_new_scalar = m1_512[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0;)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          acc->cov.a[len0 * (i + j) + k] +=
                                                  ((val1[i + j] * m1_new_scalar) *
                                                   (val0[k] * acc->m.a[k]));
                                          k++;
                                      }
                                  }
            );

            LOOP_HAVE_256(i, len1,
                          accumulate(AVX256, m1, s1, curr1, count,
                                     &acc->m.a[len0 + i],
                                     &acc->s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 8; j++)
                                  {
                                      m1_new_scalar = m1_256[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0;)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k])
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          acc->cov.a[len0 * (i + j) + k] +=
                                                  ((val1[i + j] - m1_new_scalar) *
                                                   (val0[k] - acc->m.a[k]));
                                          k++;
                                      }
                                  }
            );

            LOOP_HAVE_128(i, len1,
                          accumulate(AVX128, m1, s1, curr1, count,
                                     &acc->m.a[len0 + i],
                                     &acc->s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 4; j++)
                                  {
                                      m1_new_scalar = m1_[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0;)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                                          );

                                          acc->cov.a[len0 * (i + j) + k] +=
                                                  ((val1[i + j] - m1_new_scalar) *
                                                   (val0[k] - acc->m.a[k]));
                                          k++;
                                      }
                                  }
            );

            m1_new_scalar = acc->m.a[len0 + i] + (val1[i] - acc->m.a[len0 + i]) / acc->count;
            acc->s.a[len0 + i] += ((val1[i] - acc->m.a[len0 + i]) * (val1[i] - m1_new_scalar));

            IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&acc->m.a[len0 + i]));
            IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&acc->m.a[len0 + i]));
            IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_m_));

            acc->m.a[len0 + i] = m1_new_scalar;

            IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i]));
            IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i]));
            IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

            for(k = 0; k < len0;)
            {
                LOOP_HAVE_512(k, len0,
                              accumulate_cov(AVX512, cov, &acc->cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                );

                LOOP_HAVE_256(k, len0,
                              accumulate_cov(AVX256, cov, &acc->cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                );

                LOOP_HAVE_128(k, len0,
                              accumulate_cov(AVX128, cov, &acc->cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &acc->m.a[k]);
                );

                acc->cov.a[len0 * i + k] += ((val1[i] - m1_new_scalar) *
                                             (val0[k] - acc->m.a[k]));
                k++;
            }

            i++;
        }
    }

    return 0;
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

int __get_mean_dual_array(struct accumulator *acc, int index, float *res)
{
    if(index >= (acc->dim0 + acc->dim1))
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->m.a[index];
    return 0;
}

int __get_dev_dual_array(struct accumulator *acc, int index, float *res)
{
    if(index >= (acc->dim0 + acc->dim1))
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = sqrtf(acc->s.a[index] / (acc->count - 1));
    return 0;
}

int __get_cov_dual_array(struct accumulator *acc, int index, float *res)
{
    if(index >= (acc->dim0 * acc->dim1))
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    *res = acc->cov.a[index] / acc->count;
    return 0;
}

int __get_pearson_dual_array(struct accumulator *acc, int index, float *res)
{
    int i0, i1;
    if(index >= (acc->dim0 * acc->dim1))
    {
        err("Invalid index for accumulator\n");
        return -EINVAL;
    }

    i0 = (index % acc->dim0);
    i1 = (index / acc->dim0);

    *res = acc->cov.a[index] /
           ((acc->count - 1) *
            sqrtf(acc->s.a[i0] / (acc->count - 1)) *
            sqrtf(acc->s.a[acc->dim0 + i1] / (acc->count - 1)));
    return 0;
}

int __get_mean_dual_array_all(struct accumulator *acc, float **res)
{
    float *result = calloc(acc->dim0 + acc->dim1, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    memcpy(result, acc->m.a, acc->dim0 * acc->dim1 * sizeof(float));
    *res = result;
    return 0;
}

int __get_dev_dual_array_all(struct accumulator *acc, float **res)
{
    int i, len;
    float count;

    IF_HAVE_512(__m512 count_512);
    IF_HAVE_256(__m256 count_256);
    IF_HAVE_128(__m128 count_);

    len = acc->dim0 + acc->dim1;

    float *result = calloc(len, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    count = acc->count - 1;
    IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
    IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
    IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

    for(i = 0; i < len;)
    {
        LOOP_HAVE_512(i, len,
                      reduce_dev(AVX512, &acc->s.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_256(i, len,
                      reduce_dev(AVX256, &acc->s.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_128(i, len,
                      reduce_dev(AVX128, &acc->s.a[i],
                                 &result[i], count);
        );

        result[i] = sqrtf(acc->s.a[i] / count);
        i++;
    }

    *res = result;
    return 0;
}

int __get_cov_dual_array_all(struct accumulator *acc, float **res)
{
    int i, len;

    IF_HAVE_512(__m512 count_512);
    IF_HAVE_256(__m256 count_256);
    IF_HAVE_128(__m128 count_);

    len = acc->dim0 * acc->dim1;

    float *result = calloc(len, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->count));
    IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->count));
    IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

    for(i = 0; i < len;)
    {
        LOOP_HAVE_512(i, len,
                      reduce_cov(AVX512, &acc->cov.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_256(i, len,
                      reduce_cov(AVX256, &acc->cov.a[i],
                                 &result[i], count);
        );

        LOOP_HAVE_128(i, len,
                      reduce_cov(AVX128, &acc->cov.a[i],
                                 &result[i], count);
        );

        result[i] = acc->cov.a[i] / acc->count;
        i++;
    }

    *res = result;
    return 0;
}

int __get_pearson_dual_array_all(struct accumulator *acc, float **res)
{
    int i, j, len;
    float count, dev;

    IF_HAVE_512(__m512 count_512, dev_512);
    IF_HAVE_256(__m256 count_256, dev_256);
    IF_HAVE_128(__m128 count_, dev_);

    len = acc->dim0 * acc->dim1;

    float *result = calloc(len, sizeof(float));
    if(!result)
    {
        err("Failed to allocate result\n");
        return -ENOMEM;
    }

    count = acc->count - 1;
    IF_HAVE_128(count_ = _mm_broadcast_ss(&count));
    IF_HAVE_256(count_256 = _mm256_broadcast_ss(&count));
    IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

    for(j = 0; j < acc->dim1; j++)
    {
        dev = sqrtf(acc->s.a[acc->dim0 + j] / count);
        IF_HAVE_128(dev_ = _mm_broadcast_ss(&dev));
        IF_HAVE_256(dev_256 = _mm256_broadcast_ss(&dev));
        IF_HAVE_512(dev_512 = _mm512_broadcastss_ps(dev_));

        for(i = 0; i < acc->dim0;)
        {
            LOOP_HAVE_512(i, acc->dim0,
                          reduce_pearson(AVX512, &acc->cov.a[j * acc->dim0 + i],
                                         &acc->s.a[i], dev,
                                         &result[j * acc->dim0 + i], count);
            );

            LOOP_HAVE_256(i, acc->dim0,
                          reduce_pearson(AVX256, &acc->cov.a[j * acc->dim0 + i],
                                         &acc->s.a[i], dev,
                                         &result[j * acc->dim0 + i], count);
            );

            LOOP_HAVE_128(i, acc->dim0,
                          reduce_pearson(AVX128, &acc->cov.a[j * acc->dim0 + i],
                                         &acc->s.a[i], dev,
                                         &result[j * acc->dim0 + i], count);
            );

            result[j * acc->dim0 + i] = acc->cov.a[j * acc->dim0 + i] /
                                        (count * dev *
                                         sqrtf(acc->s.a[i] / count));
            i++;
        }
    }

    *res = result;
    return 0;
}