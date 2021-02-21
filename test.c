#include <stdio.h>
#include <stdlib.h>

#include "transform.h"
#include "libtrs.h"

int main()
{
//    struct dpa_args args = {
//            .ts_path = "/mnt/shared/c1412.trs",
//            .n_thrd = 7,
//            .n_power_models = 16
//    };
//
//    run_dpa(&args);

    struct tfm *tfm_avg, *tfm_extract_fixed, *tfm_extract_random;
    struct trace_set *source,
            *split_fixed, *split_random,
            *avg_fixed, *avg_random;
    struct trace *trace;

    tfm_average(&tfm_avg, false);
    tfm_split_tvla(&tfm_extract_fixed, true);
    tfm_split_tvla(&tfm_extract_random, false);

    ts_open(&source, "/mnt/raid0/Data/em/tvla_soc_pos1_cpu2.trs");
    ts_transform(&split_fixed, source, tfm_extract_fixed);
    ts_transform(&avg_fixed, split_fixed, tfm_avg);

    ts_transform(&split_random, source, tfm_extract_random);
    ts_transform(&avg_random, split_random, tfm_avg);

    trace_get(avg_fixed, &trace, 0, true);
    trace_get(avg_random, &trace, 0, true);
}
