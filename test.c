#include "transform.h"
#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

int main()
{
    struct trace_set *source, *aligned, *narrowed, *written;
    struct tfm *tfm_align, *tfm_small, *tfm_write;

//    int lower[] = {464};
//    int upper[] = {464 + 729};

    int lower[] = {2884};
    int upper[] = {2884 + 911};

    tfm_static_align(&tfm_align, 0.95, 1000, 3,
                     1, lower, upper);
    tfm_save(&tfm_write, "/mnt/raid0/Data/test/align");

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce.trs");
    tfm_narrow(&tfm_small, 0, 10000, 0, ts_num_samples(source));

    ts_transform(&aligned, source, tfm_align);
    ts_transform(&narrowed, aligned, tfm_small);
    ts_transform(&written, narrowed, tfm_write);

    ts_create_cache(source, 1ull * 1024 * 1024 * 1024, 16);

    ts_render(written, 4);

    ts_close(written);
    ts_close(narrowed);
    ts_close(aligned);
    ts_close(source);

//    for(i = 0; i < 10000; i++)
//    {
//        trace_get(aligned, &out, i, true);
//        trace_free(out);
//    }
}
