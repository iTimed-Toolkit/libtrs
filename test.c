#include <stdio.h>
#include <stdlib.h>

#include "transform.h"
#include "libtrs.h"

#include "dpa.h"

int main()
{
//    struct dpa_args args = {
//            .ts_path = "/mnt/raid0/Data/em/tvla_pos1_cpu2_ls_aligned_both.trs",
//            .n_thrd = 7,
//            .n_power_models = 16
//    };
//
//    run_dpa(&args);

    int i;
    struct tfm *tfm_differential, *tfm_align, *tfm_avg, *tfm_tvla;
    struct trace_set *source, *filtered, *aligned, *averaged, *broken;
    struct trace *trace;

    int lower[] = {479};
    int upper[] = {479 + 300};

    tfm_split_tvla(&tfm_tvla, TVLA_RANDOM);
    tfm_static_align(&tfm_align,
                     0.95, 500, 5,
                     1, lower, upper);
    tfm_average(&tfm_avg, PER_SAMPLE);
    tfm_dpa(&tfm_differential);

    ts_open(&source, "/mnt/raid0/Data/em/tvla_pos1_arm_ce.trs");
    ts_transform(&filtered, source, tfm_tvla);
    ts_transform(&aligned, filtered, tfm_align);
    ts_transform(&averaged, aligned, tfm_avg);
    ts_transform(&broken, aligned, tfm_differential);

    ts_create_cache(source, 8 * 1024 * 1024, 4);
    ts_create_cache(filtered, 8 * 1024 * 1024, 4);
    ts_create_cache(aligned, 1ull * 1024 * 1024 * 1024, 16);

    for(i = 0; i < ts_num_traces(broken); i++)
    {
        trace_get(broken, &trace, i, true);
    }

//    trace_get(averaged, &trace, 0, true);
//
//    for(i = 0; i < 10; i++)
//    {
//        trace_get(aligned, &trace, i, true);
//        trace_free(trace);
//    }
}
