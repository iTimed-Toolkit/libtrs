#ifndef LIBTRS_TRANSFORM_H
#define LIBTRS_TRANSFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct trace_set;
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
int tfm_narrow(struct tfm **tfm,
               int first_trace, int num_traces,
               int first_sample, int num_samples);

// Align
int tfm_static_align(struct tfm **tfm, double confidence,
                     int max_shift, size_t ref_trace, size_t num_regions,
                     int *ref_samples_lower, int *ref_samples_higher);

// Correlation

int tfm_cpa(struct tfm **tfm,
            int (*power_model)(uint8_t *, int, float *),
            int (*consumer_init)(struct trace_set *, void *),
            int (*consumer_exit)(struct trace_set *, void *),
            void *init_args);

int tfm_io_correlation(struct tfm **tfm, int granularity, int num);

typedef enum
{
    AES128_R10_HW_SBOXIN,
} aes_leakage_t;

int tfm_analyze_aes(struct tfm **tfm, bool verify_data, aes_leakage_t leakage_model);

#endif //LIBTRS_TRANSFORM_H
