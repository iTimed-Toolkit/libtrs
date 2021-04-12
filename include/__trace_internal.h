#ifndef LIBTRS___TRACE_INTERNAL_H
#define LIBTRS___TRACE_INTERNAL_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>

struct trace_cache;
struct trace_set;
struct trace;

struct trace_set
{
    // for reading from files
    FILE *ts_file;
    sem_t file_lock;

    // size params
    size_t set_id;
    size_t num_samples, num_traces;
    size_t trace_start, trace_length;

    // commonly used, buffered headers
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
    float yscale;

    size_t num_headers;
    struct th_data *headers;
    struct trace_cache *cache;

    // for transformations
    struct trace_set *prev;
    struct tfm *tfm;

    void *tfm_data;
    int (*tfm_next)(void *, int port,
                    int nargs, ...);
    void *tfm_next_arg;
};

struct trace
{
    struct trace_set *owner;
    size_t start_offset;

    char *buffered_title;
    uint8_t *buffered_data;
    float *buffered_samples;
};

typedef enum
{
    DEBUG = 0,
    WARN,
    ERR,
    CRITICAL
} log_level_t;

static log_level_t libtrace_log_level = WARN;

#define __first_arg(a, ...)     a
#define __other_arg(a, ...)     , ## __VA_ARGS__
#define __msg(level, ...)       if(level >= libtrace_log_level) {               \
                                     fprintf(stderr, "libtrace >>> %s @ %i: "   \
                                        __first_arg(__VA_ARGS__), __FUNCTION__, \
                                        __LINE__ __other_arg(__VA_ARGS__));     \
                                        fflush(stderr); }

#define debug(...)              __msg(DEBUG, __VA_ARGS__)
#define warn(...)               __msg(WARN, __VA_ARGS__)
#define err(...)                __msg(ERR, __VA_ARGS__)
#define critical(...)           __msg(CRITICAL, __VA_ARGS__)

#define TRACE_IDX(t)  (((t)->start_offset - (t)->owner->trace_start)  / \
                            (t)->owner->trace_length)

/* Trace interface */
int trace_free_memory(struct trace *t);
int trace_copy(struct trace **res, struct trace *prev);
int read_title_from_file(struct trace *t, char **title);
int read_samples_from_file(struct trace *t, float **samples);
int read_data_from_file(struct trace *t, uint8_t **data);

/* Header interface */
int read_headers(struct trace_set *ts);
int write_default_headers(struct trace_set *ts);
int write_inherited_headers(struct trace_set *ts);
int finalize_headers(struct trace_set *ts);
int free_headers(struct trace_set *ts);

/* Cache interface */
size_t __find_num_traces(struct trace_set *ts, size_t size_bytes, int assoc);
int tc_cache_manual(struct trace_cache **cache, size_t id, size_t nsets, size_t nways);
int tc_lookup(struct trace_cache *cache, size_t index, struct trace **trace, bool keep_lock);
int tc_store(struct trace_cache *cache, size_t index, struct trace *trace, bool keep_lock);
int tc_deref(struct trace_cache *cache, size_t index, struct trace *trace);
int tc_free(struct trace_cache *cache);

#endif //LIBTRS___TRACE_INTERNAL_H
