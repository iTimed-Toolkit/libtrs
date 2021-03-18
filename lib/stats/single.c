#include "statistics.h"

#include "__trace_internal.h"
#include "__stat_internal.h"
#include <errno.h>
#include <stdlib.h>


struct accumulator
{
    enum
    {
        ACC_SINGLE,
        ACC_DUAL,
        ACC_SINGLE_ARRAY,
        ACC_DUAL_ARRAY
    } acc_type;

    int dim0, dim1;

    union
    {
        float f;
        float *a;
    } acc_count;

    union
    {
        float f;
        float *a;
    } acc_m;

    union
    {
        float f;
        float *a;
    } acc_s;

    union
    {
        float f;
        float *a;
    } acc_cov;
};


int stat_create_single(struct accumulator **acc)
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

    res->acc_type = ACC_SINGLE;
    res->acc_count.f = 0;
    res->acc_m.f = 0;
    res->acc_s.f = 0;
    res->acc_cov.f = 0;

    *acc = res;
    return 0;
}

int stat_create_dual(struct accumulator **acc)
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

    res->acc_type = ACC_DUAL;
    res->acc_count.f = 0;

    res->acc_m.a = calloc(2, sizeof(float));
    if(!res->acc_m.a)
    {
        err("Failed to allocate dual m array\n");
        goto __free_acc;
    }

    res->acc_s.a = calloc(2, sizeof(float));
    if(!res->acc_s.a)
    {
        err("Failed to allocate dual s array\n");
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
    res->acc_count.f = 0;

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

    res->acc_type = ACC_DUAL_ARRAY;
    res->dim0 = num0;
    res->dim1 = num1;
    res->acc_count.f = 0;

    res->acc_m.a = calloc(num0 + num1, sizeof(float));
    if(!res->acc_m.a)
    {
        err("Failed to allocate dual m array\n");
        goto __free_acc;
    }

    res->acc_s.a = calloc(num0 + num1, sizeof(float));
    if(!res->acc_s.a)
    {
        err("Failed to allocate dual s array\n");
        goto __free_acc;
    }

    res->acc_cov.a = calloc(num0 * num1, sizeof(float));
    if(!res->acc_cov.a)
    {
        err("Failed to allocate dual cov array\n");
        goto __free_acc;
    }

    *acc = res;
    return 0;

__free_acc:
    if(res->acc_m.a)
        free(res->acc_m.a);

    if(res->acc_s.a)
        free(res->acc_s.a);

    if(res->acc_cov.a)
        free(res->acc_cov.a);

    free(res);
    return -ENOMEM;
}

int stat_accumulate_single(struct accumulator *acc, float val)
{
    float m_new;

    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->acc_type != ACC_SINGLE)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    acc->acc_count.f++;
    if(acc->acc_count.f == 1)
    {
        acc->acc_m.f = val;
        acc->acc_s.f = 0;
    }
    else
    {
        m_new = acc->acc_m.f + (val - acc->acc_m.f) / acc->acc_count.f;
        acc->acc_s.f += ((val - acc->acc_m.f) * (val - m_new));
        acc->acc_m.f = m_new;
    }

    return 0;
}

int stat_accumulate_dual(struct accumulator *acc, float val0, float val1)
{
    float m_new_0, m_new_1;

    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    if(acc->acc_type != ACC_DUAL)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    acc->acc_count.f++;
    if(acc->acc_count.f == 1)
    {
        acc->acc_m.a[0] = val0;
        acc->acc_m.a[1] = val1;

        acc->acc_s.a[0] = 0;
        acc->acc_s.a[1] = 0;
        acc->acc_cov.f = 0;
    }
    else
    {
        m_new_0 = acc->acc_m.a[0] + (val0 - acc->acc_m.a[0]) / acc->acc_count.f;
        m_new_1 = acc->acc_m.a[1] + (val1 - acc->acc_m.a[1]) / acc->acc_count.f;

        acc->acc_s.a[0] += ((val0 - acc->acc_m.a[0]) * (val0 - m_new_0));
        acc->acc_s.a[1] += ((val1 - acc->acc_m.a[1]) * (val1 - m_new_1));
        acc->acc_cov.f += ((val0 - acc->acc_m.a[0]) * (val1 - m_new_1));
        acc->acc_m.a[0] = m_new_0;
        acc->acc_m.a[1] = m_new_1;
    }

    return 0;
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

    acc->acc_count.f++;
    if(acc->acc_count.f == 1)
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
        IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->acc_count.f));
        IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->acc_count.f));
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

            m_new_scalar = acc->acc_m.a[i] + (val[i] - acc->acc_m.a[i]) / acc->acc_count.f;
            acc->acc_s.a[i] += ((val[i] - acc->acc_m.a[i]) * (val[i] - m_new_scalar));
            acc->acc_m.a[i] = m_new_scalar;
            i++;
        }
    }
    return 0;
}

int stat_accumulate_dual_array(struct accumulator *acc, float *val0, float *val1, int len0, int len1)
{
    int i, j, k;
    float m0_new_scalar, m1_new_scalar;
    float *m0_new_buffer;

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

    if(!acc || !val0 || !val1)
    {
        err("Invalid accumulator or data arrays\n");
        return -EINVAL;
    }

    if(acc->acc_type != ACC_DUAL_ARRAY)
    {
        err("Invalid accumulator type\n");
        return -EINVAL;
    }

    if(acc->dim0 != len0 || acc->dim1 != len1)
    {
        err("Invalid data dimensions\n");
        return -EINVAL;
    }

    m0_new_buffer = calloc(len1, sizeof(float));
    if(!m0_new_buffer)
    {
        err("Failed to allocate temporary memory\n");
        return -ENOMEM;
    }

    if(acc->acc_count.f == 1)
    {
        for(i = 0; i < len0;)
        {
            LOOP_HAVE_512(i, len0,
                          init_m(AVX512, &acc->acc_m.a[i], &val0[i])
            );

            LOOP_HAVE_256(i, len0,
                          init_m(AVX256, &acc->acc_m.a[i], &val0[i])
            );

            LOOP_HAVE_128(i, len0,
                          init_m(AVX128, &acc->acc_m.a[i], &val0[i])
            )

            acc->acc_m.a[i] = val0[i];
            acc->acc_s.a[i] = 0;
            i++;
        }

        for(i = 0; i < len1;)
        {
            LOOP_HAVE_512(i, len1,
                          init_m(AVX512, &acc->acc_m.a[i], &val1[i])
            );

            LOOP_HAVE_256(i, len1,
                          init_m(AVX256, &acc->acc_m.a[i], &val1[i])
            );

            LOOP_HAVE_128(i, len1,
                          init_m(AVX128, &acc->acc_m.a[i], &val1[i])
            );

            acc->acc_m.a[len0 + i] = val1[i];
            acc->acc_s.a[i] = 0;
            i++;
        }
    }
    else
    {
        IF_HAVE_128(count_ = _mm_broadcast_ss(&acc->acc_count.f));
        IF_HAVE_256(count_256 = _mm256_broadcast_ss(&acc->acc_count.f));
        IF_HAVE_512(count_512 = _mm512_broadcastss_ps(count_));

        for(i = 0; i < len0;)
        {
            LOOP_HAVE_512(i, len0,
                          accumulate(AVX512, m0, s0, curr0, count,
                                     &acc->acc_m.a[i],
                                     &acc->acc_s.a[i],
                                     &val0[i]);

                                  avx_storeu_ps(AVX512, &m0_new_buffer[i], m0_new_512);
            );

            LOOP_HAVE_256(i, len0,
                          accumulate(AVX256, m0, s0, curr0, count,
                                     &acc->acc_m.a[i],
                                     &acc->acc_s.a[i],
                                     &val0[i]);

                                  avx_storeu_ps(AVX256, &m0_new_buffer[i], m0_new_256);
            );

            LOOP_HAVE_128(i, len0,
                          accumulate(AVX128, m0, s0, curr0, count,
                                     &acc->acc_m.a[i],
                                     &acc->acc_s.a[i],
                                     &val0[i]);

                                  avx_storeu_ps(AVX128, &m0_new_buffer[i], m0_new_);
            );

            m0_new_scalar = acc->acc_m.a[i] + (val0[i] - acc->acc_m.a[i]) / acc->acc_count.f;
            acc->acc_s.a[i] += ((val0[i] - acc->acc_m.a[i]) * (val0[i] - m0_new_scalar));
            acc->acc_m.a[i] = m0_new_scalar;
            m0_new_buffer[i] = m0_new_scalar;
            i++;
        }

        for(i = 0; i < len1; i++)
        {
            LOOP_HAVE_512(i, len1,
                          accumulate(AVX512, m1, s1, curr1, count,
                                     &acc->acc_m.a[len0 + i],
                                     &acc->acc_s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 16; j++)
                                  {
                                      m1_new_scalar = m1_512[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_curr_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0; k++)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          acc->acc_cov.a[k] += ((val1[i + j] * m1_new_scalar) *
                                                                (val0[k] * m0_new_buffer[k]));
                                          k++;
                                      }
                                  }
            );

            LOOP_HAVE_256(i, len1,
                          accumulate(AVX256, m1, s1, curr1, count,
                                     &acc->acc_m.a[len0 + i],
                                     &acc->acc_s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 8; j++)
                                  {
                                      m1_new_scalar = m1_256[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_curr_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0; k++)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          acc->acc_cov.a[k] += ((val1[i + j] * m1_new_scalar) *
                                                                (val0[k] * m0_new_buffer[k]));
                                          k++;
                                      }
                                  }
            );

            LOOP_HAVE_128(i, len1,
                          accumulate(AVX128, m1, s1, curr1, count,
                                     &acc->acc_m.a[len0 + i],
                                     &acc->acc_s.a[len0 + i],
                                     &val1[i]);

                                  for(j = 0; j < 4; j++)
                                  {
                                      m1_new_scalar = m1_[j];

                                      IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
                                      IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_curr_));

                                      IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i + j]));
                                      IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

                                      for(k = 0; k < len0; k++)
                                      {
                                          LOOP_HAVE_512(k, len0,
                                                        accumulate_cov(AVX512, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_256(k, len0,
                                                        accumulate_cov(AVX256, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          LOOP_HAVE_128(k, len0,
                                                        accumulate_cov(AVX128, cov, &acc->acc_cov.a[len0 * (i + j) + k],
                                                                       cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                                          );

                                          acc->acc_cov.a[k] += ((val1[i + j] * m1_new_scalar) *
                                                                (val0[k] * m0_new_buffer[k]));
                                          k++;
                                      }
                                  }
            );

            m1_new_scalar = acc->acc_m.a[len0 + i] + (val1[i] - acc->acc_m.a[len0 + i]) / acc->acc_count.f;
            acc->acc_s.a[len0 + i] += ((val1[i] - acc->acc_m.a[len0 + i]) * (val1[i] - m1_new_scalar));
            acc->acc_m.a[len0 + i] = m1_new_scalar;

            IF_HAVE_128(cov_m_ = _mm_broadcast_ss(&m1_new_scalar));
            IF_HAVE_256(cov_m_256 = _mm256_broadcast_ss(&m1_new_scalar));
            IF_HAVE_512(cov_m_512 = _mm512_broadcastss_ps(cov_curr_));

            IF_HAVE_128(cov_curr_ = _mm_broadcast_ss(&val1[i]));
            IF_HAVE_256(cov_curr_256 = _mm256_broadcast_ss(&val1[i]));
            IF_HAVE_512(cov_curr_512 = _mm512_broadcastss_ps(cov_curr_));

            for(k = 0; k < len0; k++)
            {
                LOOP_HAVE_512(k, len0,
                              accumulate_cov(AVX512, cov, &acc->acc_cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                );

                LOOP_HAVE_256(k, len0,
                              accumulate_cov(AVX256, cov, &acc->acc_cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                );

                LOOP_HAVE_128(k, len0,
                              accumulate_cov(AVX128, cov, &acc->acc_cov.a[len0 * i + k],
                                             cov_curr, cov_m, &val0[k], &m0_new_buffer[k]);
                );

                acc->acc_cov.a[k] += ((val1[i] * m1_new_scalar) *
                                      (val0[k] * m0_new_buffer[k]));
                k++;
            }

            i++;
        }
    }

    free(m0_new_buffer);
    return 0;
}

int stat_get_mean(struct accumulator *acc, int index, float **res)
{

}

int stat_get_dev(struct accumulator *acc, int index, float **res)
{

}

int stat_get_cov(struct accumulator *acc, int index, float **res)
{

}

int stat_get_pearson(struct accumulator *acc, int index, float **res)
{

}

int stat_get_mean_all(struct accumulator *acc, float **res)
{

}

int stat_get_dev_all(struct accumulator *acc, float **res)
{

}

int stat_get_cov_all(struct accumulator *acc, float **res)
{

}

int stat_get_pearson_all(struct accumulator *acc, float **res)
{

}

