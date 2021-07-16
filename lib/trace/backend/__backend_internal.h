#ifndef LIBTRS___BACKEND_INTERNAL_H
#define LIBTRS___BACKEND_INTERNAL_H

#include "platform.h"

/* Riscure trsfile utils */
struct backend_trs_arg
{
    char *name;
    LT_FILE_TYPE *file;
    LT_SEM_TYPE file_lock;

    size_t num_headers;
    struct th_data *headers;

    bool mode;
    size_t trace_start, trace_length;
    size_t num_written, position;
};

#define MODE_READ   true
#define MODE_WRITE  false
#define TRS_ARG(ts)     ((struct backend_trs_arg *) (ts)->backend->arg)

int read_headers(struct trace_set *);
int write_default_headers(struct trace_set *);
int finalize_headers(struct trace_set *);
int free_headers(struct trace_set *);

/* Backend initializers */
int create_backend_trs(struct trace_set *, const char *);
int create_backend_ztrs(struct trace_set *, const char *);
int create_backend_net(struct trace_set *, const char *);

#endif //LIBTRS___BACKEND_INTERNAL_H