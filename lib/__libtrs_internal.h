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
    struct tfm *tfm;
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
        (res)->init = name ## _init;                  \
        (res)->title = name ## _title;                \
        (res)->data = name ## _data;                  \
        (res)->samples = name ## _samples;            \
        (res)->exit = name ## _exit;                  \
        (res)->free_title = name ## _free_title;      \
        (res)->free_data = name ## _free_data;        \
        (res)->free_samples = name ## _free_samples;  \

struct tfm
{
    int (*init)(struct trace_set *ts);
    int (*title)(struct trace *t, char **title);
    int (*data)(struct trace *t, uint8_t **data);
    int (*samples)(struct trace *t, float **samples);

    void (*exit)(struct trace_set *ts);
    void (*free_title)(struct trace *t);
    void (*free_data)(struct trace *t);
    void (*free_samples)(struct trace *t);

    void *tfm_data;
};

#endif //LIBTRS___LIBTRS_INTERNAL_H
