#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <string.h>
#include <math.h>

#include <immintrin.h>

#define TFM_DATA(tfm)   ((struct tfm_static_align *) (tfm)->tfm_data)


/**
 * Extra accumulation functions, unused but will be useful
 * for debugging if/when AVX accelerated accumulate breaks
 */

struct tfm_static_align
{
    double confidence;
    int max_shift;

    size_t ref_trace, num_regions;
    int *ref_samples_lower;
    int *ref_samples_higher;
};

int __accumulate(struct trace *t, const float *ref_samples,
                 size_t *shift_cnt, float *shift_sum,
                 float *shift_sq, float *shift_prod)
{
    int ret, c, r, i, j;
    int i_lower, i_upper;

    struct trace *curr_trace;
    float *curr_samples, curr_sq;

    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, t->start_offset, false);
    if(ret < 0)
    {
        err("Failed to get trace to align from previous trace set\n");
        return ret;
    }

    ret = trace_samples(curr_trace, &curr_samples);
    if(ret < 0)
    {
        err("Failed to get samples from trace to align\n");
        goto __free_trace;
    }

    if(!curr_samples)
    {
        ret = 1;
        goto __free_trace;
    }

    for(c = 0; c < ts_num_samples(t->owner); c++)
    {
        curr_sq = pow(curr_samples[c], 2.0f);
        for(r = 0; r < tfm->num_regions; r++)
        {
            i_lower = (c - tfm->max_shift >= tfm->ref_samples_lower[r]) ?
                      c - tfm->max_shift + 1 : (int) tfm->ref_samples_lower[r];

            i_upper = (c + tfm->max_shift < tfm->ref_samples_higher[r] ?
                       c + tfm->max_shift + 1 : (int) tfm->ref_samples_higher[r]);

            for(i = i_upper - 1; i >= i_lower; i--)
            {
                j = c - i + tfm->max_shift;

                if(j == 16)
                    fprintf(stderr, "prod j %i += c %i * i %i = %f\n", j, c, i,
                            curr_samples[c] * ref_samples[i]);

                shift_cnt[j]++;
                shift_sum[j] += curr_samples[c];
                shift_sq[j] += curr_sq;
                shift_prod[j] += curr_samples[c] * ref_samples[i];
            }
        }
    }

    ret = 0;
__free_trace:
    trace_free(curr_trace);
    return ret;
}

int __accumulate_std(struct trace *t, const float *ref_samples,
                     size_t *shift_cnt, float *shift_sum,
                     float *shift_sq, float *shift_prod)
{
    int ret, r, i, j;
    struct trace *curr_trace;
    float *curr_samples;

    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, t->start_offset, false);
    if(ret < 0)
    {
        err("Failed to get trace to align from previous trace set\n");
        return ret;
    }

    ret = trace_samples(curr_trace, &curr_samples);
    if(ret < 0)
    {
        err("Failed to get samples from trace to align\n");
        goto __free_trace;
    }

    if(!curr_samples)
    {
        ret = 1;
        goto __free_trace;
    }

    for(j = -tfm->max_shift; j < tfm->max_shift; j++)
    {
        for(r = 0; r < tfm->num_regions; r++)
        {
            for(i = tfm->ref_samples_lower[r];
                i < tfm->ref_samples_higher[r];
                i++)
            {
                if(i + j >= 0 &&
                   i + j < ts_num_samples(t->owner->prev))
                {
                    shift_cnt[tfm->max_shift + j]++;
                    shift_sum[tfm->max_shift + j] += curr_samples[i + j];
                    shift_sq[tfm->max_shift + j] += curr_samples[i + j] * curr_samples[i + j];
                    shift_prod[tfm->max_shift + j] += curr_samples[i + j] * ref_samples[i];
                }
            }
        }
    }

    ret = 0;
__free_trace:
    trace_free(curr_trace);
    return ret;
}

int __accumulate_std_avx(struct trace *t, const float *ref_samples,
                         size_t *shift_cnt, float *shift_sum,
                         float *shift_sq, float *shift_prod)
{
    int ret, num, r, i, j, k;
    struct trace *curr_trace;
    float *curr_samples, batch_ref[8], batch_curr[8], batch_sq[8], batch_prod[8];

    size_t shift_cnt_local;
    float shift_sum_local, shift_sq_local, shift_prod_local;

    __m256 curr_sum, curr_sq, curr_prod;

    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, t->start_offset, false);
    if(ret < 0)
    {
        err("Failed to get trace to align from previous trace set\n");
        return ret;
    }

    ret = trace_samples(curr_trace, &curr_samples);
    if(ret < 0)
    {
        err("Failed to get samples from trace to align\n");
        goto __free_trace;
    }

    if(!curr_samples)
    {
        ret = 1;
        goto __free_trace;
    }

    for(j = -tfm->max_shift; j < tfm->max_shift; j++)
    {
        shift_cnt_local = 0;
        shift_sum_local = shift_sq_local = shift_prod_local = 0;

        for(r = 0; r < tfm->num_regions; r++)
        {
            for(i = tfm->ref_samples_lower[r];
                i < tfm->ref_samples_higher[r];
                i++)
            {
                if(i + j >= 0 &&
                   i + j < ts_num_samples(t->owner->prev))
                {
                    num = i + 8 < tfm->ref_samples_higher[r] ?
                          8 : tfm->ref_samples_higher[r] - i;

                    memcpy(batch_curr, &curr_samples[i + j], num * sizeof(float));
                    memcpy(batch_ref, &ref_samples[i], num * sizeof(float));

                    curr_sum = _mm256_loadu_ps(batch_curr);
                    curr_sq = _mm256_mul_ps(curr_sum, curr_sum);
                    curr_prod = _mm256_mul_ps(curr_sum, _mm256_loadu_ps(batch_ref));

                    _mm256_storeu_ps(batch_sq, curr_sq);
                    _mm256_storeu_ps(batch_prod, curr_prod);

                    for(k = 0; k < num; k++)
                    {
                        shift_cnt_local++;
                        shift_sum_local += batch_curr[k];
                        shift_sq_local += batch_sq[k];
                        shift_prod_local += batch_prod[k];
                    }

                    i += (num - 1);
                }
            }
        }

        shift_cnt[tfm->max_shift + j] = shift_cnt_local;
        shift_sum[tfm->max_shift + j] = shift_sum_local;
        shift_sq[tfm->max_shift + j] = shift_sq_local;
        shift_prod[tfm->max_shift + j] = shift_prod_local;
    }

    ret = 0;

__free_trace:
    trace_free(curr_trace);
    return ret;
}