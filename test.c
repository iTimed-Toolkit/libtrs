#include <stdio.h>
#include <stdlib.h>

#include "dpa.h"

int main()
{
    struct dpa_args args = {
            .ts_path = "/mnt/shared/c1412.trs",
            .max_ram = (size_t) 4 * 1024 * 1024 * 1024,
            .n_buf_thrd = 1,
            .n_calc_thrd = 7,
            .n_power_models = 8
    };

    run_dpa(&args);
}
