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
    BLOCK_MAX,
    BLOCK_MIN,
    BLOCK_MAXABS,
    BLOCK_MINABS
} block_t;

typedef enum
{
    PORT_ECHO = 0,
    PORT_CPA_PROGRESS,
    PORT_CPA_SPLIT_PM,
    PORT_CPA_SPLIT_PM_PROGRESS
} port_t;

struct viz_args
{
    char *filename;
    int rows, cols, plots, samples;
    float rate;

    enum
    {
        ROWS,
        COLS,
        PLOTS
    } fill_order[3];
};

// System
int tfm_save(struct tfm **tfm, char *path_prefix);
int tfm_wait_on(struct tfm **tfm, port_t port);
int tfm_visualize(struct tfm **tfm, struct viz_args *args);

// Analysis
int tfm_average(struct tfm **tfm, bool per_sample);

int tfm_block_maxabs(struct tfm **tfm, bool per_sample, int blocksize);
int tfm_block_select(struct tfm **tfm, int blocksize, block_t block);

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
int tfm_io_correlation(struct tfm **tfm, int granularity, int num);

typedef enum
{
    AES128_R0_R1_HD_NOMC,
    AES128_R0_HW_SBOXOUT,
    AES128_R10_HW_SBOXIN,
} aes_leakage_t;

int tfm_analyze_aes(struct tfm **tfm, bool verify_data, aes_leakage_t leakage_model);

#endif //LIBTRS_TRANSFORM_H
