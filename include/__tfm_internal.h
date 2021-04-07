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
        (res)->title = name ## _title;                  \
        (res)->data = name ## _data;                    \
        (res)->samples = name ## _samples;              \
        (res)->exit = name ## _exit;                    \
        (res)->free_title = name ## _free_title;        \
        (res)->free_data = name ## _free_data;          \
        (res)->free_samples = name ## _free_samples;

struct tfm
{
    int (*init)(struct trace_set *ts);
    int (*init_waiter)(struct trace_set *ts, port_t port);
    size_t (*trace_size)(struct trace_set *ts);

    int (*title)(struct trace *t, char **title);
    int (*data)(struct trace *t, uint8_t **data);
    int (*samples)(struct trace *t, float **samples);

    void (*exit)(struct trace_set *ts);
    void (*free_title)(struct trace *t);
    void (*free_data)(struct trace *t);
    void (*free_samples)(struct trace *t);

    void *tfm_data;
};

int passthrough_title(struct trace *trace, char **title);
int passthrough_data(struct trace *trace, uint8_t **data);
int passthrough_samples(struct trace *trace, float **samples);

void passthrough_free_title(struct trace *t);
void passthrough_free_data(struct trace *t);
void passthrough_free_samples(struct trace *t);

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

#endif //LIBTRS___TFM_INTERNAL_H
