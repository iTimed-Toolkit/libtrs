#ifndef LIBTRS_DPA_H
#define LIBTRS_DPA_H

#include <stdlib.h>
#include "libtrs.h"

typedef int (*pm_func)(struct trace *t, size_t guess);

struct dpa_args
{
    char *ts_path;
    size_t max_ram;
    size_t n_buf_thrd;
    size_t n_calc_thrd;

    size_t n_power_models;

    // todo
    pm_func *power_model_funcs;
};

int run_dpa(struct dpa_args *arg);

#endif //LIBTRS_DPA_H
