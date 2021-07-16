#include "transform.h"
#include "trace.h"
#include "statistics.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <string.h>
#include <errno.h>
#include <math.h>

#define TFM_DATA(tfm)   ((struct cpa_args *) (tfm)->data)
#define CPA_REPORT_INTERVAL     100000
#define CPA_TITLE_SIZE          128

int __tfm_cpa_init(struct trace_set *ts)
{
    int ret;
    struct cpa_args *tfm = TFM_DATA(ts->tfm);

    ts->title_size = 0;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;

    // consumers are expected to set this
    ts->num_traces = -1;
    ts->num_samples = -1;

    ret = tfm->consumer_init(ts, tfm->init_args);
    if(ret < 0 || ts->num_traces == -1 || ts->num_samples == -1)
    {
        err("Failed to initialize consumer\n");
        return ret;
    }

    if(ts->num_samples == ts->prev->num_samples)
        tfm->num_models = 1;
    else if(ts->num_samples % ts->prev->num_samples == 0)
        tfm->num_models = (int) (ts->num_samples / ts->prev->num_samples);
    else
    {
        err("Invalid number of multi-indices provided by consumer\n");
        tfm->consumer_exit(ts, tfm->init_args);
        return -EINVAL;
    }

    return 0;
}

int __tfm_cpa_init_waiter(struct trace_set *ts, port_t port)
{
    struct cpa_args *tfm;
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    ts->title_size = CPA_TITLE_SIZE;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;

    tfm = TFM_DATA(ts->prev->tfm);
    switch(port)
    {
        case PORT_CPA_PROGRESS:
            ts->num_traces = ts_num_traces(ts->prev) *
                             ts_num_traces(ts->prev->prev) / CPA_REPORT_INTERVAL;
            ts->num_samples = ts_num_samples(ts->prev);
            break;

        case PORT_CPA_SPLIT_PM:
            ts->num_traces = tfm->num_models * ts_num_traces(ts->prev);
            ts->num_samples = ts_num_samples(ts->prev) / tfm->num_models;
            break;

        case PORT_CPA_SPLIT_PM_PROGRESS:
            ts->num_traces = tfm->num_models * ts_num_traces(ts->prev) *
                             ts_num_traces(ts->prev->prev) / CPA_REPORT_INTERVAL;
            ts->num_samples = ts_num_samples(ts->prev) / tfm->num_models;
            break;

        default:
            err("Invalid port specified: %i\n", port);
            return -EINVAL;
    }

    return 0;
}

size_t __tfm_cpa_trace_size(struct trace_set *ts)
{
    return ts->title_size + ts->num_samples * sizeof(float);
}

void __tfm_cpa_exit(struct trace_set *ts)
{
    struct cpa_args *tfm = TFM_DATA(ts->tfm);
    tfm->consumer_exit(ts, tfm->init_args);
}

int __tfm_cpa_get(struct trace *t)
{
    int i, j, ret;
    int count = 0;

    struct trace *curr;
    float *pm, *pearson;
    char title[CPA_TITLE_SIZE];

    struct accumulator *acc;
    struct cpa_args *tfm = TFM_DATA(t->owner->tfm);

    pm = calloc(tfm->num_models, sizeof(float));
    if(!pm)
    {
        err("Failed to allocate power model array\n");
        return -ENOMEM;
    }

    ret = stat_create_dual_array(&acc, ts_num_samples(t->owner->prev), tfm->num_models);
    if(ret < 0)
    {
        err("Failed to create accumulator\n");
        goto __free_pm;
    }

    for(i = 0; i < ts_num_traces(t->owner->prev); i++)
    {
        if(i % CPA_REPORT_INTERVAL == 0)
            warn("CPA %li working on trace %i\n", TRACE_IDX(t), i);

        ret = trace_get(t->owner->prev, &curr, i);
        if(ret < 0)
        {
            err("Failed to get trace at index %i\n", i);
            goto __free_accumulator;
        }

        if(curr->samples && curr->data)
        {
            for(j = 0; j < tfm->num_models; j++)
            {
                ret = tfm->power_model(curr->data,
                                       (size_t) (tfm->num_models *
                                                 TRACE_IDX(t) + j), &pm[j]);
                if(ret < 0)
                {
                    err("Failed to calculate power model for trace %i, lets skip this one\n", i);
                    goto __next_trace;
                }
            }

            ret = stat_accumulate_dual_array(acc, curr->samples, pm,
                                             ts_num_samples(t->owner->prev), j);
            if(ret < 0)
            {
                err("Failed to accumulate index %i\n", i);
                goto __free_trace;
            }

            count++;
            if(count % CPA_REPORT_INTERVAL == 0)
            {
                if(t->owner->tfm_next)
                {
                    ret = stat_get_pearson_all(acc, &pearson);
                    if(ret < 0)
                    {
                        err("Failed to get all pearson values from accumulator\n");
                        goto __free_trace;
                    }

                    debug("CPA %li pushing intermediate %li\n", TRACE_IDX(t),
                          TRACE_IDX(t) + ts_num_traces(t->owner) *
                                         (count / CPA_REPORT_INTERVAL - 1));

                    memset(title, 0, CPA_TITLE_SIZE * sizeof(char));
                    snprintf(title, CPA_TITLE_SIZE,
                             "CPA %li (%i traces)", TRACE_IDX(t), count);

                    ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_CPA_PROGRESS, 4,
                                             TRACE_IDX(t) + ts_num_traces(t->owner) *
                                                            (count / CPA_REPORT_INTERVAL - 1),
                                             title, NULL, pearson);
                    if(ret < 0)
                    {
                        err("Failed to push pearson to consumer\n");
                        goto __free_trace;
                    }

                    for(j = 0; j < tfm->num_models; j++)
                    {
                        memset(title, 0, CPA_TITLE_SIZE * sizeof(char));

                        tfm->progress_title(title, CPA_TITLE_SIZE,
                                            tfm->num_models * TRACE_IDX(t) + j,
                                            count);

                        ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_CPA_SPLIT_PM_PROGRESS, 4,
                                                 tfm->num_models * ts_num_traces(t->owner) *
                                                 (count / CPA_REPORT_INTERVAL - 1) +
                                                 tfm->num_models * TRACE_IDX(t) + j,
                                                 title, NULL,
                                                 &pearson[j * ts_num_samples(t->owner) / tfm->num_models]);
                        if(ret < 0)
                        {
                            err("Failed to push pearson to consumer\n");
                            goto __free_trace;
                        }
                    }

                    free(pearson);
                }
            }
        }
        else debug("No samples or data for index %i, skipping\n", i);

__next_trace:
        trace_free(curr);
        curr = NULL;
    }

    t->title = NULL;
    t->data = NULL;

    ret = stat_get_pearson_all(acc, &pearson);
    if(ret < 0)
    {
        err("Failed to get all pearson values from accumulator\n");
        t->samples = NULL;
    }
    else
    {
        if(t->owner->tfm_next)
        {
            for(j = 0; j < tfm->num_models; j++)
            {
                memset(title, 0, CPA_TITLE_SIZE * sizeof(char));

                tfm->progress_title(title, CPA_TITLE_SIZE,
                                    tfm->num_models * TRACE_IDX(t) + j,
                                    count);

                ret = t->owner->tfm_next(t->owner->tfm_next_arg, PORT_CPA_SPLIT_PM, 4,
                                         tfm->num_models * TRACE_IDX(t) + j,
                                         title, NULL, &pearson[j * ts_num_samples(t->owner) / tfm->num_models]);
                if(ret < 0)
                {
                    err("Failed to push pearson to consumer\n");
                    goto __free_trace;
                }
            }
        }

        t->samples = pearson;
    }

__free_trace:
    if(curr)
        trace_free(curr);

__free_accumulator:
    stat_free_accumulator(acc);

__free_pm:
    free(pm);
    return ret;
}

void __tfm_cpa_free(struct trace *t)
{
    free(t->samples);
}

int tfm_cpa(struct tfm **tfm, struct cpa_args *args)
{
    struct tfm *res;

    if(!tfm || !args)
    {
        err("Invalid transformation or cpa args\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct tfm));
    if(!res)
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_cpa);

    res->data = calloc(1, sizeof(struct cpa_args));
    if(!res->data)
    {
        free(res);
        return -ENOMEM;
    }

    memcpy(res->data, args, sizeof(struct cpa_args));
    *tfm = res;
    return 0;
}
