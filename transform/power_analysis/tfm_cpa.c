#include "transform.h"
#include "libtrace.h"

#include "__tfm_internal.h"
#include "__trace_internal.h"

#include <string.h>
#include <errno.h>
#include <math.h>

#include <immintrin.h>

#define TFM_DATA(tfm)   ((struct tfm_cpa *) (tfm)->tfm_data)

struct tfm_cpa
{
    int (*power_model)(uint8_t *data, int index, float *res);

    int (*consumer_init)(struct trace_set *, void *);
    int (*consumer_exit)(struct trace_set *, void *);
    void *init_args;
};

int __tfm_cpa_init(struct trace_set *ts)
{
    struct tfm_cpa *tfm = TFM_DATA(ts->tfm);
    ts->num_samples = ts->prev->num_samples;

    ts->input_offs = ts->input_len =
    ts->output_offs = ts->output_len =
    ts->key_offs = ts->key_len = 0;

    ts->title_size = 0;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;

    // consumers are expected to set this
    ts->num_traces = -1;
    tfm->consumer_init(ts, tfm->init_args);
    return 0;
}

size_t __tfm_cpa_trace_size(struct trace_set *ts)
{
    return ts->title_size + ts->num_samples * sizeof(float);
}

int __tfm_cpa_title(struct trace *t, char **title)
{
    *title = NULL;
    return 0;
}

int __tfm_cpa_data(struct trace *t, uint8_t **data)
{
    *data = NULL;
    return 0;
}

int __tfm_cpa_samples(struct trace *t, float **samples)
{
    int i, j, ret = 0;

    struct trace *curr;
    uint8_t *curr_data;
    float *curr_samples, *result = NULL;

    float count = 0;
    float pm, pm_m_f, pm_m_new_f, pm_s_f, tr_m_new_f;
    float *tr_m_f = NULL, *tr_s_f = NULL, *cov_f = NULL;

    __m256 curr_pm, curr_count, curr_batch,
            curr_m, curr_m_new,
            curr_s, curr_s_new,
            curr_cov, curr_cov_new;

    struct tfm_cpa *tfm = TFM_DATA(t->owner->tfm);

    result = calloc(t->owner->num_samples, sizeof(float));
    if(!result)
    {
        err("Failed to allocate memory for result samples\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_m_f = calloc(t->owner->num_samples, sizeof(float));
    if(!tr_m_f)
    {
        err("Failed to allocate memory for temporary sum array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_s_f = calloc(t->owner->num_samples, sizeof(float));
    if(!tr_s_f)
    {
        err("Failed to allocate memory for temporary square array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    cov_f = calloc(t->owner->num_samples, sizeof(float));
    if(!cov_f)
    {
        err("Failed to allocate memory for temporary product array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    pm_m_f = pm_s_f = 0;
    for(i = 0; i < ts_num_traces(t->owner->prev); i++)
    {
        if(i % 100000 == 0)
            warn("CPA working on trace %i\n", i);

        ret = trace_get(t->owner->prev, &curr, i, true);
        if(ret < 0)
        {
            err("Failed to get trace at index %i\n", i);
            goto __free_temp;
        }

        ret = trace_data_all(curr, &curr_data);
        if(ret < 0)
        {
            err("Failed to get trace data at index %i\n", i);
            goto __free_trace;
        }

        if(curr_data)
        {
            ret = trace_samples(curr, &curr_samples);
            if(ret < 0)
            {
                err("Failed to get trace samples at index %i\n", i);
                goto __free_trace;
            }
        }

        if(curr_samples && curr_data)
        {
            count++;
            ret = tfm->power_model(curr_data, TRACE_IDX(t), &pm);
            if(ret < 0)
            {
                err("Failed to calculate power model\n");
                goto __free_trace;
            }

            curr_pm = _mm256_broadcast_ss(&pm);
            curr_count = _mm256_broadcast_ss(&count);

            if(count == 1)
            {
                pm_m_f = pm;
                pm_s_f = 0;

                for(j = 0; j < ts_num_samples(t->owner->prev);)
                {
                    if(j + 8 < ts_num_samples(t->owner->prev))
                    {
                        curr_batch = _mm256_loadu_ps(&curr_samples[j]);

                        _mm256_storeu_ps(&tr_m_f[j], curr_batch);
                        _mm256_storeu_ps(&tr_s_f[j], _mm256_setzero_ps());
                        _mm256_storeu_ps(&cov_f[j], _mm256_setzero_ps());

                        j += 8;
                    }
                    else
                    {
                        tr_m_f[j] = curr_samples[j];
                        tr_s_f[j] = 0;
                        cov_f[j] = 0;

                        j++;
                    }
                }
            }
            else
            {
                pm_m_new_f = pm_m_f + (pm - pm_m_f) / count;
                pm_s_f += ((pm - pm_m_f) * (pm - pm_m_new_f));
                pm_m_f = pm_m_new_f;

                for(j = 0; j < ts_num_samples(t->owner->prev);)
                {
                    if(j + 8 < ts_num_samples(t->owner->prev))
                    {
                        curr_batch = _mm256_loadu_ps(&curr_samples[j]);
                        curr_m = _mm256_loadu_ps(&tr_m_f[j]);
                        curr_s = _mm256_loadu_ps(&tr_s_f[j]);
                        curr_cov = _mm256_loadu_ps(&cov_f[j]);

                        curr_m_new = _mm256_add_ps(curr_m,
                                                   _mm256_div_ps(
                                                           _mm256_sub_ps(curr_batch, curr_m),
                                                           curr_count
                                                   ));

                        curr_s_new = _mm256_add_ps(curr_s,
                                                   _mm256_mul_ps(
                                                           _mm256_sub_ps(curr_batch, curr_m),
                                                           _mm256_sub_ps(curr_batch, curr_m_new)
                                                   ));

                        curr_cov_new = _mm256_add_ps(curr_cov,
                                                     _mm256_mul_ps(
                                                             _mm256_sub_ps(curr_batch, curr_m),
                                                             _mm256_sub_ps(curr_pm, _mm256_broadcast_ss(&pm_m_new_f))
                                                     ));

                        _mm256_storeu_ps(&tr_m_f[j], curr_m_new);
                        _mm256_storeu_ps(&tr_s_f[j], curr_s_new);
                        _mm256_storeu_ps(&cov_f[j], curr_cov_new);
                        j += 8;
                    }
                    else
                    {
                        tr_m_new_f = tr_m_f[j] + (curr_samples[j] - tr_m_f[j]) / count;
                        tr_s_f[j] += ((curr_samples[j] - tr_m_f[j]) * (curr_samples[j] - tr_m_new_f));
                        cov_f[j] += ((curr_samples[j] - tr_m_f[j]) * (pm - pm_m_new_f));

                        tr_m_f[j] = tr_m_new_f;
                        j++;
                    }
                }
            }
        }
        else warn("No samples or data for index %i, skipping\n", i);

        trace_free(curr);
        curr = NULL;
    }

    count--;
    pm_s_f = sqrtf(pm_s_f / count);

    curr_count = _mm256_broadcast_ss(&count);
    curr_s = _mm256_broadcast_ss(&pm_s_f);

    for(i = 0; i < ts_num_samples(t->owner->prev);)
    {
        if(i + 8 < ts_num_samples(t->owner->prev))
        {
            // whew
            _mm256_storeu_ps(&result[i],
                             _mm256_div_ps(
                                     _mm256_loadu_ps(&cov_f[i]),
                                     _mm256_mul_ps(curr_count,
                                                   _mm256_mul_ps(
                                                           _mm256_sqrt_ps(
                                                                   _mm256_div_ps(
                                                                           _mm256_loadu_ps(&tr_s_f[i]),
                                                                           curr_count)),
                                                           curr_s))));
            i += 8;
        }
        else
        {
            result[i] = cov_f[i] / (count *
                                    sqrtf(tr_s_f[i] / count) *
                                    pm_s_f);
            i++;
        }
    }

    *samples = result;
    result = NULL;

__free_trace:
    if(curr)
        trace_free(curr);

__free_temp:
    if(tr_s_f)
        free(tr_s_f);

    if(tr_m_f)
        free(tr_m_f);

    if(cov_f)
        free(cov_f);

    if(result)
        free(result);

    return ret;
}

void __tfm_cpa_exit(struct trace_set *ts)
{
    struct tfm_cpa *tfm = TFM_DATA(ts->tfm);
    tfm->consumer_exit(ts, tfm->init_args);
}

void __tfm_cpa_free_title(struct trace *t)
{}

void __tfm_cpa_free_data(struct trace *t)
{}

void __tfm_cpa_free_samples(struct trace *t)
{
    free(t->buffered_samples);
}

int tfm_cpa(struct tfm **tfm,
            int (*power_model)(uint8_t *, int, float *),
            int (*consumer_init)(struct trace_set *, void *),
            int (*consumer_exit)(struct trace_set *, void *),
            void *init_args)
{
    struct tfm *res;

    if(!power_model || !consumer_init)
    {
        err("Invalid power model or consumer initialization function\n");
        return -EINVAL;
    }

    res = calloc(1, sizeof(struct tfm));
    if(!res)
        return -ENOMEM;

    ASSIGN_TFM_FUNCS(res, __tfm_cpa);

    res->tfm_data = calloc(1, sizeof(struct tfm_cpa));
    if(!res->tfm_data)
    {
        free(res);
        return -ENOMEM;
    }

    TFM_DATA(res)->power_model = power_model;
    TFM_DATA(res)->consumer_init = consumer_init;
    TFM_DATA(res)->consumer_exit = consumer_exit;
    TFM_DATA(res)->init_args = init_args;

    *tfm = res;
    return 0;
}
