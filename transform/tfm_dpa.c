#include "transform.h"
#include "libtrs.h"

#include "__tfm_internal.h"
#include "__libtrs_internal.h"

#include <string.h>
#include <errno.h>
#include <math.h>

#include <immintrin.h>

#define TFM_DATA(tfm)   ((struct tfm_dpa *) (tfm)->tfm_data)

struct tfm_dpa
{
    float (*power_model)(uint8_t *data, int index);
};

int __tfm_dpa_init(struct trace_set *ts)
{
    ts->num_samples = ts->prev->num_samples;
    ts->num_traces = 32;

    ts->input_offs = ts->input_len =
    ts->output_offs = ts->output_len =
    ts->key_offs = ts->key_len = 0;

    ts->title_size = strlen("Key X = XX") + 1;
    ts->data_size = 0;
    ts->datatype = DT_FLOAT;
    ts->yscale = 1.0f;
    return 0;
}

size_t __tfm_dpa_trace_size(struct trace_set *ts)
{
    return ts->title_size + ts->num_samples * sizeof(float);
}


int __tfm_dpa_title(struct trace *t, char **title)
{
    char *res = calloc(strlen("Key X = XX") + 1, sizeof(char));
    if(!res)
    {
        err("Failed to allocate memory for trace title\n");
        return -ENOMEM;
    }

    if(TRACE_IDX(t) / 256 < 16)
    {
        sprintf(res, "Key %lX = %02lX", TRACE_IDX(t) / 256, TRACE_IDX(t) % 256);
        *title = res;
        return 0;
    }
    else
    {
        err("Trace offset is in inconsistent state\n");
        free(res);
        *title = NULL;
        return -EINVAL;
    }
}

int __tfm_dpa_data(struct trace *t, uint8_t **data)
{
    *data = NULL;
    return 0;
}

int __tfm_dpa_samples(struct trace *t, float **samples)
{
    int i, j, ret = 0;

    struct trace *curr;
    uint8_t *curr_data;
    float *curr_samples, *result = NULL;

    float count;
    float pm, pm_sum_f, pm_sq_f, pm_avg_f, pm_dev_f;
    float tr_avg_f, tr_dev_f, prod_f;
    float *tr_sum_f = NULL, *tr_sq_f = NULL;

    __m256 curr_batch, curr_pm;
    __m256 curr_count, curr_prod;
    __m256 tr_sum, tr_sq, tr_avg, tr_dev;
    __m256 pm_sum, pm_avg, pm_dev;

    struct tfm_dpa *tfm = TFM_DATA(t->owner->tfm);

    result = calloc(t->owner->num_samples, sizeof(float));
    if(!result)
    {
        err("Failed to allocate memory for result samples\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_sum_f = calloc(t->owner->num_samples, sizeof(float));
    if(!tr_sum_f)
    {
        err("Failed to allocate memory for temporary sum array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    tr_sq_f = calloc(t->owner->num_samples, sizeof(float));
    if(!tr_sq_f)
    {
        err("Failed to allocate memory for temporary square array\n");
        ret = -ENOMEM;
        goto __free_temp;
    }

    pm_sum_f = pm_sq_f = 0;
    for(i = 0; i < ts_num_traces(t->owner->prev); i++)
    {
        if(i % 100000 == 0)
            warn("DPA working on trace %i\n", i);
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

            pm = tfm->power_model(curr_data, TRACE_IDX(t));
            pm_sum_f += pm;
            pm_sq_f += powf(pm, 2);

            curr_pm = _mm256_broadcast_ss(&pm);
            for(j = 0; j < ts_num_samples(t->owner->prev);)
            {
                if(j + 8 < ts_num_samples(t->owner->prev))
                {
                    curr_batch = _mm256_loadu_ps(&curr_samples[j]);
                    _mm256_storeu_ps(&tr_sum_f[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&tr_sum_f[j]),
                                             curr_batch));
                    _mm256_storeu_ps(&tr_sq_f[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&tr_sq_f[j]),
                                             _mm256_mul_ps(curr_batch, curr_batch)));
                    _mm256_storeu_ps(&result[j],
                                     _mm256_add_ps(
                                             _mm256_loadu_ps(&result[j]),
                                             _mm256_mul_ps(curr_pm, curr_batch)));
                    j += 8;
                }
                else
                {
                    tr_sum_f[j] += curr_samples[j];
                    tr_sq_f[j] += powf(curr_samples[j], 2);
                    result[j] += (pm * curr_samples[j]);
                    j++;
                }
            }
        }
        else warn("No samples or data for index %i, skipping\n", i);

        trace_free(curr);
        curr = NULL;
    }

    pm_avg_f = pm_sum_f / count;
    pm_dev_f = pm_sq_f / count;
    pm_dev_f -= (pm_avg_f * pm_avg_f);
    pm_dev_f = sqrtf(pm_dev_f);

    curr_count = _mm256_broadcast_ss(&count);
    pm_sum = _mm256_broadcast_ss(&pm_sum_f);
    pm_avg = _mm256_broadcast_ss(&pm_avg_f);
    pm_dev = _mm256_broadcast_ss(&pm_dev_f);

    for(i = 0; i < ts_num_samples(t->owner);)
    {
        if(i + 8 < ts_num_samples(t->owner))
        {
            tr_sum = _mm256_loadu_ps(&tr_sum_f[i]);
            tr_sq = _mm256_loadu_ps(&tr_sq_f[i]);
            curr_prod = _mm256_loadu_ps(&result[i]);

            tr_avg = _mm256_div_ps(tr_sum, curr_count);
            tr_dev = _mm256_div_ps(tr_sq, curr_count);
            tr_dev = _mm256_sub_ps(tr_dev, _mm256_mul_ps(tr_avg, tr_avg));
            tr_dev = _mm256_sqrt_ps(tr_dev);

            curr_prod = _mm256_sub_ps(curr_prod, _mm256_mul_ps(pm_sum, tr_avg));
            curr_prod = _mm256_sub_ps(curr_prod, _mm256_mul_ps(tr_sum, pm_avg));
            curr_prod = _mm256_add_ps(curr_prod, _mm256_mul_ps(curr_count,
                                                               _mm256_mul_ps(tr_avg,
                                                                             pm_avg)));
            curr_prod = _mm256_div_ps(curr_prod, _mm256_mul_ps(curr_count,
                                                               _mm256_mul_ps(tr_dev,
                                                                             pm_dev)));
            _mm256_storeu_ps(&result[i], curr_prod);
            i += 8;
        }
        else
        {
            tr_avg_f = tr_sum_f[i] / count;
            tr_dev_f = tr_sq_f[i] / count;
            tr_dev_f -= (tr_avg_f * tr_avg_f);
            tr_dev_f = sqrtf(tr_dev_f);

            prod_f = result[i];
            prod_f -= pm_sum_f * tr_avg_f;
            prod_f -= tr_sum_f[i] * pm_avg_f;
            prod_f += (count * tr_avg_f * pm_avg_f);
            prod_f /= (count * tr_dev_f * pm_dev_f);

            result[i] = prod_f;
            i++;
        }
    }

    *samples = result;
    result = NULL;

__free_trace:
    if(curr)
        trace_free(curr);

__free_temp:
    if(tr_sq_f)
        free(tr_sq_f);

    if(tr_sum_f)
        free(tr_sum_f);

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

static const unsigned char sbox_inv[16][16] =
        {
                {0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb},
                {0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb},
                {0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e},
                {0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25},
                {0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92},
                {0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84},
                {0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06},
                {0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b},
                {0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73},
                {0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e},
                {0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b},
                {0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4},
                {0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f},
                {0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef},
                {0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61},
                {0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d}
        };

static inline int hamming_weight(unsigned char n)
{
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);

    return (char) n;
}

static inline int hamming_distance(unsigned char n, unsigned char p)
{
    return hamming_weight(n ^ p);
}

float power_model(uint8_t *data, int index)
{
//    int byte_index = index / 16, byte_guess = index % 256;
//
//    uint8_t state = data[16 + byte_index] ^(uint8_t) byte_guess;
//    state = sbox_inv[state >> 4][state & 0xF];
//
//    return (float) hamming_distance(state, data[16 + ((byte_index + 4 * (byte_index % 4)) % 16)]);

    return (float) hamming_weight(data[index]);
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
