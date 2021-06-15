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

typedef enum
{
    PORT_ECHO = 0,
    PORT_CPA_PROGRESS,
    PORT_CPA_SPLIT_PM,
    PORT_CPA_SPLIT_PM_PROGRESS
} port_t;

typedef enum
{
    ROWS = 0,
    COLS,
    PLOTS
} fill_order_t;

typedef enum
{
    AES128
} verify_t;

typedef enum
{
    SUMMARY_AVG,
    SUMMARY_DEV,
    SUMMARY_MIN,
    SUMMARY_MAX
} summary_t;

typedef enum
{
    ALONG_NUM,
    ALONG_DATA,
} filter_t;

typedef union
{
    int num;
} filter_param_t;

struct viz_args
{
    char *filename;
    int rows, cols, plots, samples;
    fill_order_t order[3];
};

// System
int tfm_save(struct tfm **tfm, char *path_prefix);
int tfm_synchronize(struct tfm **tfm, int max_distance);
int tfm_wait_on(struct tfm **tfm, port_t port, size_t bufsize);
int tfm_visualize(struct tfm **tfm, struct viz_args *args);

// Analysis
int tfm_average(struct tfm **tfm, bool per_sample);
int tfm_verify(struct tfm **tfm, verify_t which);

int tfm_reduce_along(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param);
int tfm_select_along(struct tfm **tfm, summary_t stat, filter_t along, filter_param_t param);

// Traces
int tfm_split_tvla(struct tfm **tfm, bool which);
int tfm_narrow(struct tfm **tfm,
               int first_trace, int num_traces,
               int first_sample, int num_samples);
int tfm_append(struct tfm **tfm, const char *path);

// Align
int tfm_static_align(struct tfm **tfm, double confidence,
                     int max_shift, size_t ref_trace, size_t num_regions,
                     int *ref_samples_lower, int *ref_samples_higher);

// Correlation
int tfm_io_correlation(struct tfm **tfm, bool verify_data, int granularity, int num);

typedef enum
{
    AES128_R0_R1_HD_NOMC = 0,
    AES128_RO_HW_ADDKEY_OUT,
    AES128_R0_HW_SBOX_OUT,
    AES128_R10_OUT_HD,
    AES128_R10_HW_SBOXIN,
} aes_leakage_t;

int tfm_aes_intermediate(struct tfm **tfm, aes_leakage_t leakage_model);
int tfm_aes_knownkey(struct tfm **tfm);

#endif //LIBTRS_TRANSFORM_H
