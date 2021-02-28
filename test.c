#include <stdio.h>
#include <stdlib.h>

#include "transform.h"
#include "libtrs.h"

#include "dpa.h"

int main()
{
//    int i;
    struct tfm *tfm_differential, *tfm_align, *tfm_avg, *tfm_tvla, *tfm_write;
    struct trace_set *source, *filtered, *aligned, *written;
    struct trace *trace;

    int lower[] = {479};
    int upper[] = {479 + 300};

    tfm_split_tvla(&tfm_tvla, TVLA_RANDOM);
    tfm_static_align(&tfm_align,
                     0.95, 500, 5,
                     1, lower, upper);
    tfm_average(&tfm_avg, PER_SAMPLE);
    tfm_dpa(&tfm_differential);
    tfm_save(&tfm_write, "/tmp/out");

    ts_open(&source, "/mnt/raid0/Data/em/tvla_pos1_arm_ce.trs");
    ts_transform(&filtered, source, tfm_tvla);
    ts_transform(&aligned, filtered, tfm_align);
//    ts_transform(&averaged, aligned, tfm_avg);
    ts_transform(&written, aligned, tfm_write);

    ts_create_cache(source, 1ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(filtered, 1ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(aligned, 1ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(written, 1ull * 1024 * 1024 * 1024, 16);

//    ts_render(written, 12);
    trace_get(written, &trace, 0, true);

//    ts_open(&source, "/mnt/raid0/Data/test.trs");
//    ts_dump_headers(source);
}
