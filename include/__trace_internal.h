#ifndef LIBTRS___TRACE_INTERNAL_H
#define LIBTRS___TRACE_INTERNAL_H

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

struct trace_cache;
struct trace_set;
struct trace;

typedef enum datatype
{
    DT_BYTE = 0x1, DT_SHORT = 0x2,
    DT_INT = 0x4, DT_FLOAT = 0x14,
    DT_NONE = 0xFF
} datatype_t;

struct trace_set
{
    // size params
    size_t set_id;
    size_t num_samples, num_traces;

    // commonly used, buffered headers
    size_t title_size, data_size;
    datatype_t datatype;
    int xoffs;
    float xscale, yscale;

    struct backend_intf *backend;
    struct trace_cache *cache;

    // for transformations
    struct trace_set *prev;
    struct tfm *tfm;

    void *tfm_state;
    int (*tfm_next)(void *, int port,
                    int nargs, ...);
    void *tfm_next_arg;
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
#define __msg(level, ...)       { if(level >= libtrace_log_level) {               \
                                     fprintf(stderr, "libtrace >>> %s @ %i: "   \
                                        __first_arg(__VA_ARGS__), __FUNCTION__, \
                                        __LINE__ __other_arg(__VA_ARGS__));     \
                                        fflush(stderr); }}

#define debug(...)              __msg(DEBUG, __VA_ARGS__)
#define warn(...)               __msg(WARN, __VA_ARGS__)
#define err(...)                __msg(ERR, __VA_ARGS__)
#define critical(...)           __msg(CRITICAL, __VA_ARGS__)

#define TRACE_IDX(t)  (t)->index

/* Trace interface */
int trace_free_memory(struct trace *t);
int trace_copy(struct trace **res, struct trace *prev);

/* Backend interface */
struct backend_intf
{
    int (*open)(struct trace_set *);
    int (*create)(struct trace_set *);
    int (*close)(struct trace_set *);

    int (*read)(struct trace *);
    int (*write)(struct trace *);
    void *arg;
};

int create_backend(struct trace_set *ts, const char *name);

/* Cache interface */
size_t __find_num_traces(struct trace_set *ts, size_t size_bytes, int assoc);
int tc_cache_manual(struct trace_cache **cache, size_t id, size_t nsets, size_t nways);
int tc_lookup(struct trace_cache *cache, size_t index, struct trace **trace, bool keep_lock);
int tc_store(struct trace_cache *cache, size_t index, struct trace *trace, bool keep_lock);
int tc_deref(struct trace_cache *cache, size_t index, struct trace *trace);
int tc_free(struct trace_cache *cache);

#endif //LIBTRS___TRACE_INTERNAL_H
