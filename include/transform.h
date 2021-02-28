#ifndef LIBTRS_TRANSFORM_H
#define LIBTRS_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>

struct tfm;

#define PER_SAMPLE      true
#define PER_TRACE       false

#define TVLA_FIXED      true
#define TVLA_RANDOM     false

// IO
int tfm_save(struct tfm **tfm, char *path_prefix);

// Analysis
int tfm_average(struct tfm **tfm, bool per_sample);

// Traces
int tfm_split_tvla(struct tfm **tfm, bool which);

// Align
int tfm_static_align(struct tfm **tfm, double confidence,
                     int max_shift, size_t ref_trace, size_t num_regions,
                     int *ref_samples_lower, int *ref_samples_higher);

// Correlation

typedef enum
{
    AES128_HW_
} dpa_cipher_model_t;

typedef enum
{
    HD_PT_K0,
} dpa_model_t;

int tfm_dpa(struct tfm **tfm);

#endif //LIBTRS_TRANSFORM_H
