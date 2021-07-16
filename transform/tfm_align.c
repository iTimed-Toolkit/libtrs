#include "transform.h"
#include "trace.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define TFM_DATA(tfm)   ((struct tfm_static_align *) (tfm)->data)

struct tfm_static_align
{
    match_region_t match;
    int max_shift;
};

int __tfm_static_align_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

int __tfm_static_align_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_static_align_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_static_align_exit(struct trace_set *ts)
{}

int __do_align(struct trace *t, double *best_conf, int *best_shift)
{
    int ret, i;
    int shift_valid_lower, shift_valid_upper;
    float *pearson, *temp;

    struct trace *ref_trace, *curr_trace;
    struct accumulator *acc;
    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = trace_get(t->owner->prev, &curr_trace, TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to get trace to align from previous trace set\n");
        return ret;
    }

    if(!curr_trace->samples)
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

    ret = trace_get(t->owner->prev, &ref_trace, tfm->match.ref_trace);
    if(ret < 0)
    {
        err("Failed to get reference trace from previous trace set\n");
        goto __free_accumulator;
    }

    if(!ref_trace->samples)
    {
        err("No samples for reference trace\n");
        ret = -EINVAL;
        goto __free_ref;
    }

    shift_valid_lower = 0;
    shift_valid_upper = 2 * tfm->max_shift;

    for(i = tfm->match.lower; i < tfm->match.upper; i++)
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
               &curr_trace->samples[i + shift_valid_lower - tfm->max_shift],
               (shift_valid_upper - shift_valid_lower) * sizeof(float));

        ret = stat_accumulate_dual_array(acc, temp, &ref_trace->samples[i], 2 * tfm->max_shift, 1);
        if(ret < 0)
        {
            err("Failed to accumulate\n");
            goto __free_ref;
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

int __tfm_static_align_get(struct trace *t)
{
    int ret;
    double best_conf = 0;
    int best_shift = 0;
    float *result = NULL;

    struct trace *prev_trace;
    struct tfm_static_align *tfm = TFM_DATA(t->owner->tfm);

    ret = __do_align(t, &best_conf, &best_shift);
    if(ret < 0)
    {
        err("Failed to align trace\n");
        goto __done;
    }

    if(TRACE_IDX(t) % 1000 == 0)
        warn("Trace %li, best confidence %f for shift %i\n", TRACE_IDX(t), best_conf, best_shift);

    if(best_conf >= tfm->match.confidence)
    {
        result = calloc(ts_num_samples(t->owner), sizeof(float));
        if(!result)
        {
            err("Failed to allocate sample buffer for aligned trace\n");
            ret = -ENOMEM;
            goto __done;
        }

        // these should never fail, since they succeeded in __do_align above
        ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t));
        if(ret < 0)
        {
            err("Failed to get trace to align from previous trace sets\n");
            goto __free_result;
        }

        if(!prev_trace->samples)
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
            memcpy(result, &prev_trace->samples[best_shift], (ts_num_samples(t->owner) - best_shift) * sizeof(float));
            memcpy(&result[ts_num_samples(t->owner) - best_shift], prev_trace->samples, best_shift * sizeof(float));
        }
        else if(best_shift < 0)
        {
            memcpy(result, &prev_trace->samples[ts_num_samples(t->owner) + best_shift], -1 * best_shift * sizeof(float));
            memcpy(&result[-1 * best_shift], prev_trace->samples, (ts_num_samples(t->owner) + best_shift) * sizeof(float));
        }
        else memcpy(result, prev_trace->samples, ts_num_samples(t->owner) * sizeof(float));
    }
    else goto __done;

    ret = copy_title(t, prev_trace);
    if(ret >= 0)
        ret = copy_data(t, prev_trace);

__free_prev_trace:
    trace_free(prev_trace);

__free_result:
    if(ret < 0)
    {
        passthrough_free(t);
        free(result);
        result = NULL;
    }

__done:
    t->samples = result;
    return ret;
}

void __tfm_static_align_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_static_align(struct tfm **tfm, match_region_t *match, int max_shift)
{
    struct tfm *res = NULL;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    ASSIGN_TFM_FUNCS(res, __tfm_static_align);

    res->data = calloc(1, sizeof(struct tfm_static_align));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation variables\n");
        return -ENOMEM;
    }

    memcpy(&TFM_DATA(res)->match, match, sizeof(match_region_t));
    TFM_DATA(res)->max_shift = max_shift;

    *tfm = res;
    return 0;
}
