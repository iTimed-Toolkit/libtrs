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

    size_t set_id;
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

    struct trace_cache *cache;
};

struct trace
{
    struct trace_set *owner;
    size_t start_offset;

    char *buffered_title;
    uint8_t *buffered_data;
    float *buffered_samples;
};

#define __first_arg(a, ...)     a
#define __other_arg(a, ...)     , ## __VA_ARGS__
#define err(...)                fprintf(stderr, "libtrs >>> %s @ %i: "         \
                                        __first_arg(__VA_ARGS__), __FUNCTION__, \
                                        __LINE__ __other_arg(__VA_ARGS__))

#if SUPPORT_PTHREAD
#define ts_lock(set, out)   \
    if(sem_wait(&(set)->file_lock) < 0) out

#define ts_unlock(set, out) \
    if(sem_post(&(set)->file_lock) < 0) out
#else
#define ts_lock(set, out)   ;
#define ts_unlock(set, out) ;
#endif

int trace_free_memory(struct trace *t);

int init_headers(struct trace_set *ts);

int free_headers(struct trace_set *ts);

int tc_lookup(struct trace_set *ts, size_t index, struct trace **trace);

int tc_store(struct trace_set *ts, size_t index, struct trace *trace);

int tc_deref(struct trace_set *ts, size_t index, struct trace *trace);

int tc_free(struct trace_set *ts);

#endif //LIBTRS___LIBTRS_INTERNAL_H