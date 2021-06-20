#include "transform.h"
#include "trace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "statistics.h"

#include <errno.h>
#include <math.h>
#include <float.h>

#define TFM_DATA(tfm)   ((struct tfm_match *) (tfm)->data)

struct tfm_match
{
    match_region_t first, last, pattern;
    int avg_len, max_dev;
};

int __tfm_match_init(struct trace_set *ts)
{
    struct tfm_match *tfm = TFM_DATA(ts->tfm);

    ts->num_samples = ts->prev->num_samples -
                      (tfm->pattern.upper - tfm->pattern.lower);
    ts->num_traces = ts->prev->num_traces;

    ts->title_size = ts->prev->title_size;
    ts->data_size = ts->prev->data_size;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;
    return 0;
}

int __tfm_match_init_waiter(struct trace_set *ts, port_t port)
{
    err("No ports to register\n");
    return -EINVAL;
}

size_t __tfm_match_trace_size(struct trace_set *ts)
{
    return ts_trace_size(ts->prev);
}

void __tfm_match_exit(struct trace_set *ts)
{}

sem_t print_lock;

#define ZSCORE_95       1.960f
#define ZSCORE_99       2.576f

int __tfm_match_get(struct trace *t)
{
    int ret, i, num;

    struct accumulator *acc;
    struct trace *prev_trace, *ref_trace;
    struct tfm_match *tfm = TFM_DATA(t->owner->tfm);
    num = tfm->pattern.upper - tfm->pattern.lower;

    ret = trace_get(t->owner->prev, &ref_trace, tfm->pattern.ref_trace);
    if(ret < 0)
    {
        err("Failed to get reference trace\n");
        return ret;
    }

    ret = trace_get(t->owner->prev, &prev_trace, TRACE_IDX(t));
    if(ret < 0)
    {
        err("Failed to get trace to match\n");
        goto __free_ref;
    }

    t->title = NULL;
    t->data = NULL;
    t->samples = NULL;

    if(ref_trace->samples && prev_trace->samples)
    {
        ret = stat_create_dual_array(&acc, (int) t->owner->num_samples, 1);
        if(ret < 0)
        {
            err("Failed to create accumulator\n");
            goto __free_prev;
        }

        for(i = 0; i < num; i++)
        {
            ret = stat_accumulate_dual_array(acc, &prev_trace->samples[i],
                                             &ref_trace->samples[tfm->pattern.lower + i],
                                             (int) t->owner->num_samples, 1);
            if(ret < 0)
            {
                err("Failed to accumulate\n");
                goto __free_prev;
            }
        }

        ret = copy_title(t, prev_trace);
        if(ret >= 0)
            ret = copy_data(t, prev_trace);
        if(ret >= 0)
            ret = stat_get_pearson_all(acc, &t->samples);

        if(ret < 0)
        {
            err("Failed to get some trace data\n");
            goto __free_prev;
        }

        stat_free_accumulator(acc);
        stat_create_single(&acc);

        int last_index = -1, count_true = 0, count_found = 0;
        float gap, max_diff_from_whole = -FLT_MAX, mean, dev;

        t->samples[0] = fabsf(t->samples[0]);
        t->samples[t->owner->num_samples - 1] = fabsf(t->samples[t->owner->num_samples - 1]);

        sem_acquire(&print_lock);

        err("gap report for trace %li\n", TRACE_IDX(t));

        // first build known traces
        for(i = 1; i < t->owner->num_samples - 1; i++)
        {
            t->samples[i] = fabsf(t->samples[i]);

            // detect local maxima across threshold
            if(t->samples[i - 1] < t->samples[i] &&
               t->samples[i + 1] < t->samples[i] &&
               t->samples[i] >= tfm->pattern.confidence)
            {
                if(last_index != -1)
                {
                    if(i - last_index >= tfm->avg_len - tfm->max_dev &&
                       i - last_index < tfm->avg_len + tfm->max_dev)
                    {
                        count_true++;
                        stat_accumulate_single(acc, (float) (i - last_index));
                    }
                }

                last_index = i;
            }
        }

        stat_get_mean(acc, 0, &mean);
        stat_get_dev(acc, 0, &dev);

        // then predict gaps: front to back
        last_index = -1;
        for(i = 1; i < t->owner->num_samples - 1; i++)
        {
            if(t->samples[i - 1] < t->samples[i] &&
               t->samples[i + 1] < t->samples[i] &&
               t->samples[i] >= tfm->pattern.confidence)
            {
                if(last_index != -1)
                {
                    if(i - last_index < tfm->avg_len - tfm->max_dev ||
                       i - last_index >= tfm->avg_len + tfm->max_dev)
                    {
                        gap = (float) (i - last_index) / mean;
                        if(fabsf(gap - roundf(gap)) > max_diff_from_whole)
                        {
                            max_diff_from_whole = fabsf(gap - roundf(gap));
                            err("found new max diff in gap %f from %i to %i: %f\n",
                                gap, last_index, i, max_diff_from_whole);
                        }

                        if(fabsf(gap - roundf(gap)) <= 0.1f)
                            count_found += (int) roundf(gap);
                        else
                        {
                            err("rejecting gap from %i to %i\n", last_index, i);
                        }
                    }
                }

                last_index = i;
            }
        }

        err("count %i mean %f dev %f, max diff %f\n", count_true + count_found, mean, dev, max_diff_from_whole);
        sem_release(&print_lock);
    }

    ret = 0;
__free_prev:
    trace_free(prev_trace);

__free_ref:
    trace_free(ref_trace);
    return ret;
}

void __tfm_match_free(struct trace *t)
{
    passthrough_free(t);
}

int tfm_match(struct tfm **tfm, match_region_t *first, match_region_t *last,
              match_region_t *pattern, int avg_len, int max_dev)
{
    struct tfm *res;
    if(!(first->ref_trace == last->ref_trace &&
         first->ref_trace == pattern->ref_trace))
    {
        err("First, last, and pattern need to share a ref trace\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct tfm));
    if(!res)
    {
        err("Failed to allocate memory for transformation\n");
        return -ENOMEM;
    }

    sem_init(&print_lock, 0, 1);

    ASSIGN_TFM_FUNCS(res, __tfm_match);

    res->data = calloc(1, sizeof(struct tfm_match));
    if(!res->data)
    {
        err("Failed to allocate memory for transformation data\n");
        free(res);
        return -ENOMEM;
    }

    memcpy(&TFM_DATA(res)->first, first, sizeof(match_region_t));
    memcpy(&TFM_DATA(res)->last, last, sizeof(match_region_t));
    memcpy(&TFM_DATA(res)->pattern, pattern, sizeof(match_region_t));
    TFM_DATA(res)->avg_len = avg_len;
    TFM_DATA(res)->max_dev = max_dev;

    *tfm = res;
    return 0;
}