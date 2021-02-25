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

    struct tfm *tfm_align, *tfm_avg, *tfm_tvla;
    struct trace_set *source, *filtered, *aligned, *averaged;
    struct trace *trace;

    int lower[] = {479};
    int upper[] = {479 + 300};

    tfm_static_align(&tfm_align,
                     0.95, 500, 5,
                     1, lower, upper);
    tfm_average(&tfm_avg, true);
    tfm_split_tvla(&tfm_tvla, TVLA_RANDOM);

    ts_open(&source, "/mnt/raid0/Data/em/tvla_pos1_arm_ce.trs");
    ts_transform(&filtered, source, tfm_tvla);
    ts_transform(&aligned, filtered, tfm_align);
    ts_transform(&averaged, aligned, tfm_avg);

    ts_create_cache(source, 1ull  * 1024 * 1024 * 1024, 16);
    ts_create_cache(filtered, 1ull  * 1024 * 1024 * 1024, 16);
//    ts_create_cache(aligned, 1ull  * 1024 * 1024 * 1024, 16);

    trace_get(averaged, &trace, 0, true);
}
