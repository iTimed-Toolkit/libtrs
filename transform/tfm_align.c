#include "transform.h"
#include "trace.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

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

int __tfm_static_align_init_waiter(struct trace_set *ts, int port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_static_align_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

int __do_align(struct trace *t, double *best_conf, int *best_shift)
{
    int ret, r, i;
    int shift_valid_lower, shift_valid_upper;
    float *ref_samples, *curr_samples, *pearson, *temp;

    struct trace *ref_trace, *curr_trace;
    struct accumulator *acc;
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
        trace_free(curr_trace);
        return ret;
    }

    if(!curr_samples)
    {
        debug("No valid previous trace\n");
        *best_conf = 0;
        return 0;
    }

    temp = calloc(2 * tfm->max_shift, sizeof(float));
    if(!temp)
    {
        err("Unable to allocate temp memory\n");
        ret = -ENOMEM;
        goto __free_trace;
    }

    ret = stat_create_dual_array(&acc, 2 * tfm->max_shift, 1);
    if(ret < 0)
    {
        err("Failed to create accumulator\n");
        goto __free_temp;
    }

    ret = trace_get(t->owner->prev, &ref_trace, tfm->ref_trace, false);
    if(ret < 0)
    {
        err("Failed to get reference trace from previous trace set\n");
        goto __free_accumulator;
    }

    ret = trace_samples(ref_trace, &ref_samples);
    if(ret < 0)
    {
        err("Failed to get reference trace samples from previous trace\n");
        goto __free_ref;
    }

    if(!ref_samples)
    {
        err("No samples for reference trace\n");
        ret = -EINVAL;
        goto __free_ref;
    }

    shift_valid_lower = 0;
    shift_valid_upper = 2 * tfm->max_shift;

    for(r = 0; r < tfm->num_regions; r++)
    {
        for(i = tfm->ref_samples_lower[r];
            i < tfm->ref_samples_higher[r]; i++)
        {
            if(i - tfm->max_shift < 0)
            {
                if(tfm->max_shift - i > shift_valid_lower)
                    shift_valid_lower = tfm->max_shift - i;
            }

            if(i + tfm->max_shift >= ts_num_samples(t->owner))
            {
                if(ts_num_samples(t->owner) - i + tfm->max_shift < shift_valid_upper)
                    shift_valid_upper = (int) ts_num_samples(t->owner) - i + tfm->max_shift;
            }

            if(shift_valid_lower > 0)
                memset(temp, 0, shift_valid_lower * sizeof(float));

            if(shift_valid_upper < 2 * tfm->max_shift)
                memset(&temp[shift_valid_upper], 0, (2 * tfm->max_shift - shift_valid_upper) * sizeof(float));

            memcpy(&temp[shift_valid_lower],
                   &curr_samples[i + shift_valid_lower - tfm->max_shift],
                   (shift_valid_upper - shift_valid_lower) * sizeof(float));

            ret = stat_accumulate_dual_array(acc, temp, &ref_samples[i], 2 * tfm->max_shift, 1);
            if(ret < 0)
            {
                err("Failed to accumulate\n");
                goto __free_ref;
            }
        }
    }

    ret = stat_get_pearson_all(acc, &pearson);
    if(ret < 0)
    {
        err("Failed to get pearson from accumulator\n");
        goto __free_ref;
    }

    for(i = shift_valid_lower; i < shift_valid_upper; i++)
    {
        if(fabsf(pearson[i]) > *best_conf)
        {
            *best_conf = fabsf(pearson[i]);
            *best_shift = i - tfm->max_shift;
        }
    }

    free(pearson);
    ret = 0;
__free_ref:
    trace_free(ref_trace);

__free_accumulator:
    stat_free_accumulator(acc);

__free_temp:
    free(temp);

__free_trace:
    trace_free(curr_trace);

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
    int ret;
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

    if(TRACE_IDX(t) % 1000 == 0)
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

        if(best_shift > 0)
        {
            memcpy(result, &shift[best_shift], (ts_num_samples(t->owner) - best_shift) * sizeof(float));
            memcpy(&result[ts_num_samples(t->owner) - best_shift], shift, best_shift * sizeof(float));
        }
        else if(best_shift < 0)
        {
            memcpy(result, &shift[ts_num_samples(t->owner) + best_shift], -1 * best_shift * sizeof(float));
            memcpy(&result[-1 * best_shift], shift, (ts_num_samples(t->owner) + best_shift) * sizeof(float));
        }
        else memcpy(result, shift, ts_num_samples(t->owner) * sizeof(float));
    }
    else goto __out;

__free_prev_trace:
    trace_free(prev_trace);

__free_result:
    if(ret < 0)
    {
        free(result);
        result = NULL;
    }

__out:
    *samples = result;
    return ret;
}

void __tfm_static_align_exit(struct trace_set *ts)
{}

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
