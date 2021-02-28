#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <immintrin.h>

#define TFM_DATA(tfm)   ((struct tfm_static_align *) (tfm)->tfm_data)

struct tfm_static_align
{
    double confidence;
    int max_shift;

    size_t ref_trace, num_regions;
    int *ref_samples_lower;
    int *ref_samples_higher;
};

int __tfm_static_align_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->input_offs = ts->prev->input_offs;
    ts->input_len = ts->prev->input_len;
    ts->output_offs = ts->prev->output_offs;
    ts->output_len = ts->prev->output_len;
    ts->key_offs = ts->prev->key_offs;
    ts->key_len = ts->prev->key_len;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

size_t __tfm_static_align_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __accumulate_avx(struct trace *t, const float *ref_samples,
                     float *shift_cnt, float *shift_sum,
                     float *shift_sq, float *shift_prod)
{
    int ret, num, c, r, i, j, k;
    int i_lower, i_upper;

    struct trace *curr_trace;
    float *curr_samples, batch_ref[8], batch_curr[8], batch_sq[8], batch_prod[8];
    __m256 curr_sum, curr_sq, curr_prod;

    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, TRACE_IDX(t), false);
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
        curr_sum = _mm256_broadcast_ss(&curr_samples[c]);
        curr_sq = _mm256_mul_ps(curr_sum, curr_sum);
        _mm256_storeu_ps(batch_curr, curr_sum);
        _mm256_storeu_ps(batch_sq, curr_sq);

        for(r = 0; r < tfm->num_regions; r++)
        {
            i_lower = (c - tfm->max_shift >= tfm->ref_samples_lower[r]) ?
                      c - tfm->max_shift + 1 : (int) tfm->ref_samples_lower[r];

            i_upper = (c + tfm->max_shift < tfm->ref_samples_higher[r] ?
                       c + tfm->max_shift + 1 : (int) tfm->ref_samples_higher[r]);

            for(i = i_upper - 1; i >= i_lower;)
            {
                j = c - i + tfm->max_shift;
                num = (j + 8 <= c - i_lower + tfm->max_shift) ?
                      8 : i - i_lower + 1;

                memcpy(batch_ref, &ref_samples[i - num + 1], num * sizeof(float));
                curr_prod = _mm256_mul_ps(curr_sum, _mm256_loadu_ps(batch_ref));
                curr_prod = _mm256_permute_ps(
                        _mm256_permute2f128_ps(curr_prod, curr_prod, 1),
                        27);

                for(k = 0; k < num; k++)
                    shift_cnt[j + k]++;

                if(num == 8)
                {
                    _mm256_storeu_ps(&shift_sum[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&shift_sum[j]),
                                             curr_sum));

                    _mm256_storeu_ps(&shift_sq[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&shift_sq[j]),
                                             curr_sq));

                    _mm256_storeu_ps(&shift_prod[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&shift_prod[j]),
                                             curr_prod));
                }
                else
                {
                    _mm256_storeu_ps(batch_prod, curr_prod);
                    for(k = 0; k < num; k++)
                    {
                        shift_sum[j + k] += batch_curr[k];
                        shift_sq[j + k] += batch_sq[k];
                        shift_prod[j + k] += batch_prod[8 - num + k];
                    }
                }

                i -= num;
            }
        }
    }

    ret = 0;
__free_trace:
    trace_free(curr_trace);
    return ret;
}

void __calculate_ref(struct tfm_static_align *tfm, float *ref_samples,
                     size_t *ref_count_res, __m256 *ref_sum_res,
                     __m256 *ref_avg_res, __m256 *ref_dev_res)
{
    int i, r;
    float batch[8];

    size_t ref_count = 0;
    __m256 ref_curr, ref_avg;
    __m256 ref_sum = _mm256_setzero_ps();
    __m256 ref_dev = _mm256_setzero_ps();

    for(r = 0; r < tfm->num_regions; r++)
    {
        for(i = (int) tfm->ref_samples_lower[r];
            i < (int) tfm->ref_samples_higher[r];)
        {
            if((int) tfm->ref_samples_higher[r] - i >= 8)
            {
                ref_curr = _mm256_loadu_ps(&ref_samples[i]);
                ref_sum = _mm256_add_ps(ref_sum, ref_curr);
                ref_dev = _mm256_add_ps(ref_dev,
                                        _mm256_mul_ps(ref_curr,
                                                      ref_curr));
                ref_count += 8;
                i += 8;
            }
            else break;
        }

        for(; i < (int) tfm->ref_samples_higher[r]; i++)
        {
            ref_sum[(i - tfm->ref_samples_lower[r]) % 8] += ref_samples[i];
            ref_dev[(i - tfm->ref_samples_lower[r]) % 8] += pow(ref_samples[i], 2.0);
            ref_count++;
        }
    }

    _mm256_storeu_ps(batch, ref_sum);
    for(i = 1; i < 8; i++)
        batch[0] += batch[i];

    ref_sum = _mm256_broadcast_ss(&batch[0]);
    batch[0] /= ref_count;
    ref_avg = _mm256_broadcast_ss(&batch[0]);

    _mm256_storeu_ps(batch, ref_dev);
    for(i = 1; i < 8; i++)
        batch[0] += batch[i];
    batch[0] /= ref_count;

    ref_dev = _mm256_broadcast_ss(&batch[0]);
    ref_dev = _mm256_sub_ps(ref_dev, _mm256_mul_ps(ref_avg, ref_avg));
    ref_dev = _mm256_sqrt_ps(ref_dev);

    *ref_count_res = ref_count;
    *ref_sum_res = ref_sum;
    *ref_avg_res = ref_avg;
    *ref_dev_res = ref_dev;
}

void __calculate_curr(float *shift_cnt, float *shift_sum,
                      float *shift_sq, float *shift_prod,
                      __m256 ref_sum, __m256 ref_avg, __m256 ref_dev,
                      int j, __m256 *corr)
{
    __m256 curr_count, curr_sum, curr_sq, curr_prod, curr_avg, curr_dev;

    curr_count = _mm256_loadu_ps(&shift_cnt[j]);
    curr_sum = _mm256_loadu_ps(&shift_sum[j]);
    curr_sq = _mm256_loadu_ps(&shift_sq[j]);
    curr_prod = _mm256_loadu_ps(&shift_prod[j]);

    curr_avg = _mm256_div_ps(curr_sum, curr_count);
    curr_dev = _mm256_div_ps(curr_sq, curr_count);
    curr_dev = _mm256_sub_ps(curr_dev, _mm256_mul_ps(curr_avg, curr_avg));
    curr_dev = _mm256_sqrt_ps(curr_dev);

    curr_prod = _mm256_sub_ps(curr_prod, _mm256_mul_ps(ref_sum, curr_avg));
    curr_prod = _mm256_sub_ps(curr_prod, _mm256_mul_ps(curr_sum, ref_avg));
    curr_prod = _mm256_add_ps(curr_prod, _mm256_mul_ps(curr_count,
                                                       _mm256_mul_ps(curr_avg,
                                                                     ref_avg)));
    curr_prod = _mm256_div_ps(curr_prod, _mm256_mul_ps(curr_count,
                                                       _mm256_mul_ps(curr_dev,
                                                                     ref_dev)));
    *corr = curr_prod;
}

int __do_align(struct trace *t, double *best_conf, int *best_shift)
{
    int ret, i, j;
    float *ref_samples;

    size_t ref_count;
    float batch[8];
    __m256 ref_sum, ref_avg, ref_dev, curr_prod;

    float *shift_cnt = NULL;
    float *shift_sum = NULL, *shift_sq = NULL, *shift_prod = NULL;
    float curr_avg, curr_dev, product;

    struct trace *ref_trace;
    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    shift_cnt = calloc(2 * tfm->max_shift, sizeof(float));
    if(!shift_cnt)
    {
        err("Failed to allocate count buffer\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_sum = calloc(2 * tfm->max_shift, sizeof(float));
    if(!shift_sum)
    {
        err("Failed to allocate sum buffer\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_sq = calloc(2 * tfm->max_shift, sizeof(float));
    if(!shift_sq)
    {
        err("Failed to allocate square buffer\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_prod = calloc(2 * tfm->max_shift, sizeof(float));
    if(!shift_prod)
    {
        err("Failed to allocate product buffer\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    ret = trace_get(t->owner->prev, &ref_trace, tfm->ref_trace, false);
    if(ret < 0)
    {
        err("Failed to get reference trace from previous trace set\n");
        return ret;
    }

    ret = trace_samples(ref_trace, &ref_samples);
    if(ret < 0)
    {
        err("Failed to get reference trace samples from previoust race\n");
        goto __free_ref;
    }

    if(!ref_samples)
    {
        *best_conf = 0;
        goto __free_ref;
    }

    __calculate_ref(tfm, ref_samples, &ref_count,
                    &ref_sum, &ref_avg, &ref_dev);

    ret = __accumulate_avx(t, ref_samples,
                           shift_cnt, shift_sum,
                           shift_sq, shift_prod);
    if(ret < 0)
    {
        err("Failed to accumulate samples for trace to shift\n");
        goto __free_ref;
    }
    else if(ret == 1)
    {
        // special condition, trace not found
        *best_conf = 0;
        ret = 0;
        goto __free_ref;
    }

    for(j = 0; j < 2 * tfm->max_shift;)
    {
        if(j + 8 < 2 * tfm->max_shift)
        {
            __calculate_curr(shift_cnt, shift_sum, shift_sq, shift_prod,
                             ref_sum, ref_avg, ref_dev,
                             j, &curr_prod);

            _mm256_storeu_ps(batch, curr_prod);
            for(i = 0; i < 8; i++)
            {
                if(fabs(batch[i]) > *best_conf &&
                   shift_cnt[j + i] == ref_count)
                {
                    *best_conf = fabs(batch[i]);
                    *best_shift = i + j - tfm->max_shift;
                }
            }

            j += 8;
        }
        else
        {
            curr_avg = shift_sum[j] / shift_cnt[j];
            curr_dev = shift_sq[j] / shift_cnt[j];
            curr_dev -= (curr_avg * curr_avg);
            curr_dev = sqrt(curr_dev);

            product = shift_prod[j];
            product -= ref_sum[0] * curr_avg;
            product -= shift_sum[j] * ref_avg[0];
            product += shift_cnt[j] * curr_avg * ref_avg[0];
            product /= (shift_cnt[j] * curr_dev * ref_dev[0]);

            if(product > *best_conf &&
               shift_cnt[j] == ref_count)
            {
                *best_conf = product;
                *best_shift = j - tfm->max_shift;
            }

            j++;
        }
    }

    ret = 0;
__free_ref:
    trace_free(ref_trace);

__free_temp:
    if(shift_cnt)
        free(shift_cnt);

    if(shift_sum)
        free(shift_sum);

    if(shift_sq)
        free(shift_sq);

    if(shift_prod)
        free(shift_prod);

    return ret;
}

int __tfm_static_align_title(struct trace *t, char **title)
{
    return passthrough_title(t, title);
}

int __tfm_static_align_data(struct trace *t, uint8_t **data)
{
    return passthrough_data(t, data);
}

int __tfm_static_align_samples(struct trace *t, float **samples)
{
    int i, ret;
    double best_conf = 0;
    int best_shift = 0;
    float *result = NULL, *shift;

    struct trace *prev_trace;
    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = __do_align(t, &best_conf, &best_shift);
    if(ret < 0)
    {
        err("Failed to align trace\n");
        goto __out;
    }

    warn("Trace %li, best confidence %f for shift %i\n", TRACE_IDX(t), best_conf, best_shift);
    if(best_conf >= tfm->confidence)
    {
        result = calloc(ts_num_samples(t->owner), sizeof(float));
        if(!result)
        {
            err("Failed to allocate sample buffer for aligned trace\n");
            ret = -ENOMEM;
            goto __out;
        }

        // these should never fail, since they succeeded in __do_align above
        ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t), false);
        if(ret < 0)
        {
            err("Failed to get trace to align from previous trace sets\n");
            goto __free_result;
        }

        ret = trace_samples(prev_trace, &shift);
        if(ret < 0)
        {
            err("Failed to get samples from trace to align\n");
            goto __free_prev_trace;
        }

        if(!shift)
        {
            // typically this would just mean the previous transformation does
            // not create a trace for this index (not an error) but in this case
            // this is actually an error (see above)

            err("Trace to align is not defined for this index, but was when doing alignment\n");
            ret = -ENODATA;
            goto __free_prev_trace;
        }

        // todo this can probably be optimized to 1-2 memcpys
        for(i = 0; i < ts_num_samples(t->owner); i++)
        {
            if(i + best_shift < 0)
                result[i] = shift[i + best_shift + ts_num_samples(t->owner)];
            else if(i + best_shift >= ts_num_samples(t->owner))
                result[i] = shift[i + best_shift - ts_num_samples(t->owner)];
            else
                result[i] = shift[i + best_shift];
        }
    }
    else goto __out;

__free_prev_trace:
    trace_free(prev_trace);

__free_result:
    if(ret < 0 && result)
    {
        free(result);
        result = NULL;
    }

__out:
    *samples = result;
    return ret;
}

void __tfm_static_align_exit(struct trace_set *ts)
{

}

void __tfm_static_align_free_title(struct trace *t)
{
    passthrough_free_title(t);
}

void __tfm_static_align_free_data(struct trace *t)
{
    passthrough_free_data(t);
}

void __tfm_static_align_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_static_align(struct tfm **tfm, double confidence,
                     int max_shift, size_t ref_trace, size_t num_regions,
                     int *ref_samples_lower, int *ref_samples_higher)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_static_align);

    res->tfm_data = calloc(1, sizeof(struct tfm_static_align));
    if(!res->tfm_data)
    {
        err("Failed to allocate memory for transformation variables\n");
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->confidence = confidence;
    TFM_DATA(res)->max_shift = max_shift;
    TFM_DATA(res)->ref_trace = ref_trace;
    TFM_DATA(res)->num_regions = num_regions;
    TFM_DATA(res)->ref_samples_lower = ref_samples_lower;
    TFM_DATA(res)->ref_samples_higher = ref_samples_higher;

    *tfm = res;
    return 0;
}