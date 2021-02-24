#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <stdlib.h>
#include <errno.h>
#include <math.h>

#include <assert.h>

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

int __accumulate(struct trace *t, const float *ref_samples,
                 size_t *shift_cnt, double *shift_sum,
                 double *shift_sq, double *shift_prod)
{
    int ret, r, i;
    int index, i_lower, i_upper;
    double curr_sq;

    struct trace *curr_trace;
    float *curr_samples;

    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, t->start_offset, false);
    if(ret < 0)
        return ret;

    ret = trace_samples(curr_trace, &curr_samples);
    if(ret < 0)
        goto __free_trace;

    if(!curr_samples)
    {
        ret = 1;
        goto __free_trace;
    }

    // todo optimize! probably from a caching perspective
    for(index = 0; index < t->owner->num_samples; index++)
    {
        curr_sq = pow(curr_samples[index], 2);
        for(r = 0; r < tfm->num_regions; r++)
        {
            i_lower = (index - tfm->max_shift >= tfm->ref_samples_lower[r]) ?
                      index - tfm->max_shift + 1 : (int) tfm->ref_samples_lower[r];

            i_upper = (index + tfm->max_shift < tfm->ref_samples_higher[r] ?
                       index + tfm->max_shift : (int) tfm->ref_samples_higher[r]);

            for(i = i_lower; i < i_upper; i++)
            {
                shift_cnt[index - i + tfm->max_shift]++;
                shift_sum[index - i + tfm->max_shift] += curr_samples[index];
                shift_sq[index - i + tfm->max_shift] += curr_sq;
                shift_prod[index - i + tfm->max_shift] += curr_samples[index] * ref_samples[i];
            }
        }
    }

    ret = 0;
__free_trace:
    trace_free(curr_trace);
    return ret;
}

int __do_align(struct trace *t, double *best_conf, int *best_shift)
{
    int ret, r, i, j;
    float *ref_samples;

    size_t ref_count, curr_count;
    double ref_sum, ref_sq, ref_avg, ref_dev;
    double curr_sum, curr_sq, curr_avg, curr_dev;
    double product;

    size_t *shift_cnt = NULL;
    double *shift_sum = NULL, *shift_sq = NULL, *shift_prod = NULL;

    struct trace *ref_trace;
    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    shift_cnt = calloc(2 * tfm->max_shift, sizeof(size_t));
    if(!shift_cnt)
    {
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_sum = calloc(2 * tfm->max_shift, sizeof(double));
    if(!shift_sum)
    {
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_sq = calloc(2 * tfm->max_shift, sizeof(double));
    if(!shift_sq)
    {
        ret = -ENOMEM;
        goto __free_temp;
    }

    shift_prod = calloc(2 * tfm->max_shift, sizeof(double));
    if(!shift_prod)
    {
        ret = -ENOMEM;
        goto __free_temp;
    }

    ret = trace_get(t->owner->prev, &ref_trace, tfm->ref_trace, false);
    if(ret < 0)
        return ret;

    ret = trace_samples(ref_trace, &ref_samples);
    if(ret < 0)
        goto __free_ref;

    if(!ref_samples)
    {
        *best_conf = 0;
        goto __free_ref;
    }

    ref_count = 0;
    ref_sum = 0;
    ref_sq = 0;

    for(r = 0; r < tfm->num_regions; r++)
    {
        for(i = (int) tfm->ref_samples_lower[r];
            i < (int) tfm->ref_samples_higher[r];
            i++)
        {
            ref_count++;
            ref_sum += ref_samples[i];
            ref_sq += pow(ref_samples[i], 2);
        }
    }

    ref_avg = ref_sum / ref_count;
    ref_dev = ref_sq / ref_count;
    ref_dev -= (ref_avg * ref_avg);
    ref_dev = sqrt(ref_dev);

    ret = __accumulate(t, ref_samples,
                       shift_cnt, shift_sum,
                       shift_sq, shift_prod);
    if(ret < 0)
        goto __free_ref;
    else if(ret == 1)
    {
        // special condition, trace not found
        *best_conf = 0;
        ret = 0;
        goto __free_ref;
    }

    for(j = 0; j < 2 * tfm->max_shift; j++)
    {
        curr_count = shift_cnt[j];
        curr_sum = shift_sum[j];
        curr_sq = shift_sq[j];
        product = shift_prod[j];

        curr_avg = curr_sum / curr_count;
        curr_dev = curr_sq / curr_count;
        curr_dev -= (curr_avg * curr_avg);
        curr_dev = sqrt(curr_dev);

        product -= ref_sum * curr_avg;
        product -= curr_sum * ref_avg;
        product += curr_count * curr_avg * ref_avg;
        product /= (curr_count * curr_dev * ref_dev);

        if(product > *best_conf)
        {
            *best_conf = product;
            *best_shift = j - tfm->max_shift;
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
        goto __out;

    if(best_conf >= tfm->confidence)
    {
        result = calloc(t->owner->num_samples, sizeof(float));
        if(!result)
        {
            ret = -ENOMEM;
            goto __out;
        }

        // these should never fail, since they succeeded in __do_align above
        ret = trace_get(t->owner->prev, &prev_trace, t->start_offset, false);
        if(ret < 0)
            goto __free_result;

        ret = trace_samples(prev_trace, &shift);
        if(ret < 0)
            goto __free_prev_trace;

        if(!shift)
        {
            // typically this would just mean the previous transformation does
            // not create a trace for this index (not an error) but in this case
            // this is actually an error (see above)
            ret = -ENODATA;
            goto __free_prev_trace;
        }

        for(i = 0; i < t->owner->num_samples; i++)
        {
            if(i + best_shift < 0)
                result[i] = shift[i + best_shift + t->owner->num_samples];
            else if(i + best_shift >= t->owner->num_samples)
                result[i] = shift[i + best_shift - t->owner->num_samples];
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
    free(t->buffered_title);
}

void __tfm_static_align_free_data(struct trace *t)
{
    free(t->buffered_data);
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
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_static_align);

    res->tfm_data = calloc(1, sizeof(struct tfm_static_align));
    if(!res->tfm_data)
    {
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
};