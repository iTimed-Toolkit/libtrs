#ifndef LIBTRS___LIBTRS_INTERNAL_H
#define LIBTRS___LIBTRS_INTERNAL_H

#include <stdio.h>
#include <stdint.h>
#include <semaphore.h>

#define SUPPORT_PTHREAD     1

struct trace_set
{
    FILE *ts_file;
#if SUPPORT_PTHREAD
    sem_t file_lock;
#endif

    size_t num_samples, num_traces;
    size_t title_size, data_size;

    size_t input_offs, input_len,
            output_offs, output_len,
            key_offs, key_len;

    enum datatype
    {
        DT_BYTE = 0x1, DT_SHORT = 0x2,
        DT_INT = 0x4, DT_FLOAT = 0x14,
        DT_NONE = 0xFF
    } datatype;
    size_t trace_start, trace_length;
    float yscale;

    size_t num_headers;
    struct th_data *headers;

    // for transformations
    struct trace_set *prev;
    void *tfm;
};

struct trace
{
    struct trace_set *owner;
    size_t start_offset;

    char *buffered_title;
    uint8_t *buffered_data;
    float *buffered_samples;
};

#define ASSIGN_TFM_FUNCS(res, name)                       \
        (res)->gen.init = name ## _init;                  \
        (res)->gen.title = name ## _title;                \
        (res)->gen.data = name ## _data;                  \
        (res)->gen.samples = name ## _samples;            \
        (res)->gen.exit = name ## _exit;                  \
        (res)->gen.free_title = name ## _free_title;      \
        (res)->gen.free_data = name ## _free_data;        \
        (res)->gen.free_samples = name ## _free_samples;  \

struct tfm_generic
{
    int (*init)(struct trace_set *ts);
    int (*title)(struct trace *t, char **title);
    int (*data)(struct trace *t, uint8_t **data);
    int (*samples)(struct trace *t, float **samples);

    void (*exit)(struct trace_set *ts);
    void (*free_title)(struct trace *t);
    void (*free_data)(struct trace *t);
    void (*free_samples)(struct trace *t);
};

#endif //LIBTRS___LIBTRS_INTERNAL_H
