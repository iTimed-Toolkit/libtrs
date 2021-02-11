#include <stdio.h>
#include <stdlib.h>

#include "dpa.h"

int main()
{
    struct dpa_args args = {
            .ts_path = "/mnt/shared/c1412.trs",
            .n_thrd = 7,
            .n_power_models = 16
    };

    run_dpa(&args);
}
