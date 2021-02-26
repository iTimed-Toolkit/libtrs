#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <string.h>
#include <errno.h>
#include <math.h>

#define TFM_DATA(tfm)   ((struct tfm_dpa *) (tfm)->tfm_data)

struct tfm_dpa
{
    float (*power_model)(uint8_t *data);
};

int __tfm_dpa_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = 16 * 256;

    ts->input_offs = ts->input_len =
    ts->output_offs = ts->output_len =
    ts->key_offs = ts->key_len = 0;

    ts->title_size = strlen("Key X = XX");
    ts->data_size = 0;
    ts->datatype = ts->prev->datatype;
    ts->yscale = ts->prev->yscale;
    return 0;
}

size_t __tfm_dpa_trace_size(struct trace_set *ts)
{
    return ts->title_size + ts->num_samples * sizeof(float);
}


int __tfm_dpa_title(struct trace *t, char **title)
{
    char *res = calloc(strlen("Key X = XX"), sizeof(char));
    if(!res)
    {
        err("Failed to allocate memory for trace title\n");
        return -ENOMEM;
    }

    sprintf(res, "Key %lX = %02lX", t->start_offset / 256, t->start_offset % 256);
    *title = res;
    return 0;
}

int __tfm_dpa_data(struct trace *t, uint8_t **data)
{
    *data = NULL;
    return 0;
}

int __tfm_dpa_samples(struct trace *t, float **samples)
{
    int i, j, ret;
    struct trace *curr;
    struct tfm_dpa *tfm = TFM_DATA(t->owner->tfm);

    uint8_t *curr_data;
    float *curr_samples, *result = NULL, pm;

    double *tr_sum = NULL, *tr_sq = NULL;
    double pm_sum, pm_sq;

    result = calloc(t->owner->num_samples, sizeof(float));
    if(!result)
    {
        err("Failed to allocate memory for result samples\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_sum = calloc(t->owner->num_samples, sizeof(double));
    if(!tr_sum)
    {
        err("Failed to allocate memory for temporary sum array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_sq = calloc(t->owner->num_samples, sizeof(double));
    if(!tr_sq)
    {
        err("Failed to allocate memory for temporary square array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    for(i = 0; i < ts_num_traces(t->owner->prev); i++)
    {
        warn("DPA working on trace %i\n", i);
        ret = trace_get(t->owner->prev, &curr, i, true);
        if(ret < 0)
        {
            err("Failed to get trace at index %i\n", i);
            goto __out;
        }

        ret = trace_data_all(curr, &curr_data);
        if(ret < 0)
        {
            err("Failed to get trace data at index %i\n", i);
            goto __out;
        }

        if(curr_data)
        {
            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
            {
                err("Failed to get trace samples at index %i\n", i);
                goto __out;
            }
        }

        if(curr_samples && curr_data)
        {
            pm = tfm->power_model(curr_data);
            pm_sum += pm;
            pm_sq += pow(pm, 2);

            for(j = 0; j < ts_num_samples(t->owner->prev); j++)
            {
                tr_sum[j] += curr_samples[j];
                tr_sq[j] += pow(curr_samples[j], 2);
                result[j] += (pm * curr_samples[j]);
            }
        }
        else debug("No samples or data for index %i, skipping\n", i);

        trace_free(curr);
    }

__out:
__free_temp:
    if(tr_sq)
        free(tr_sq);

    if(tr_sum)
        free(tr_sum);

    if(result)
        free(result);

    return ret;
}

void __tfm_dpa_exit(struct trace_set *ts)
{

}

void __tfm_dpa_free_title(struct trace *t)
{
    free(t->buffered_title);
}

void __tfm_dpa_free_data(struct trace *t)
{

}

void __tfm_dpa_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

float power_model(uint8_t *data)
{
    return 0;
}

int tfm_dpa(struct tfm **tfm)
{
    struct tfm *res;
    res = calloc(1, sizeof(struct tfm));
    if(!res)
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_dpa);

    res->tfm_data = calloc(1, sizeof(struct tfm_dpa));
    if(!res->tfm_data)
    {
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->power_model = power_model;

    *tfm = res;
    return 0;
}