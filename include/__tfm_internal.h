#ifndef LIBTRS___TFM_INTERNAL_H
#define LIBTRS___TFM_INTERNAL_H

struct trace_set;
struct trace;

#include "transform.h"

#include <stdint.h>
#include <stdlib.h>

#define ASSIGN_TFM_FUNCS(res, name)                     \
        (res)->init = name ## _init;                    \
        (res)->init_waiter = name ## _init_waiter;      \
        (res)->trace_size = name ## _trace_size;        \
        (res)->exit = name ## _exit;                    \
        (res)->get = name ## _get;                      \
        (res)->free = name ## _free;

struct tfm
{
    int (*init)(struct trace_set *ts);
    int (*init_waiter)(struct trace_set *ts, port_t port);
    size_t (*trace_size)(struct trace_set *ts);
    void (*exit)(struct trace_set *ts);

    int (*get)(struct trace *t);
    void (*free)(struct trace *t);

    void *data;
};

int copy_title(struct trace *to, struct trace *from);
int copy_data(struct trace *to, struct trace *from);
int copy_samples(struct trace *to, struct trace *from);

int passthrough(struct trace *trace);
void passthrough_free(struct trace *t);

struct cpa_args
{
    int (*power_model)(uint8_t *, int, float *);
    int num_models;

    int (*consumer_init)(struct trace_set *, void *);
    int (*consumer_exit)(struct trace_set *, void *);
    void (*progress_title)(char *, int, size_t, int);
    void *init_args;
};

int tfm_cpa(struct tfm **tfm, struct cpa_args *args);

struct block_args
{
    int (*consumer_init)(struct trace_set *, void *);
    int (*consumer_exit)(struct trace_set *, void *);

    int (*initialize)(struct trace *, void **block, void *arg);
    bool (*trace_interesting)(struct trace *, void *arg);
    bool (*trace_matches)(struct trace *, void *block, void *arg);
    int (*accumulate)(struct trace *, void *block, void *arg);
    int (*finalize)(struct trace *, void *block, void *arg);

    enum
    {
        DONE_LISTLEN,
        DONE_SINGULAR
    } criteria;

    void *arg;
};

int tfm_block(struct tfm **tfm, struct block_args *arg);

#endif //LIBTRS___TFM_INTERNAL_H
