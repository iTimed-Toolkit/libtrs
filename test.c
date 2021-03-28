#include "transform.h"
#include "trace.h"

int main()
{
    struct trace_set *source, *narrowed, *broken, *progress, *written, *written_progress;
    struct tfm *tfm_io, *tfm_small, *tfm_wait, *tfm_write, *tfm_write_progress;
    struct render *render;

    tfm_io_correlation(&tfm_io, 32, 8);
    tfm_wait_on(&tfm_wait, PORT_CPA_PROGRESS);
    tfm_save(&tfm_write, "/mnt/raid0/Data/test/out");
    tfm_save(&tfm_write_progress, "/mnt/raid0/Data/test/progress");

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce_aligned.trs");
//    tfm_narrow(&tfm_small, 0, 10000000, 0, ts_num_samples(source));

//    ts_transform(&narrowed, source, tfm_small);
    ts_transform(&broken, source, tfm_io);
    ts_transform(&progress, broken, tfm_wait);
    ts_transform(&written, broken, tfm_write);
    ts_transform(&written_progress, progress, tfm_write_progress);

    ts_create_cache(source, 1ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(progress, 1ull * 1024 * 1024 * 1024, 16);

    ts_render_async(written_progress, 1, &render);
    ts_render(written, 8);
    ts_render_join(render);

    ts_close(written_progress);
    ts_close(written);
    ts_close(progress);
    ts_close(broken);
    ts_close(source);
}
