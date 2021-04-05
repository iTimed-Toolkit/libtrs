#include "transform.h"
#include "trace.h"

int main()
{
    struct trace_set *source, *broken,
            *progress, *split, *written, *visualized,
            *written_progress, *written_split, *max_abs, *max_trace;

    struct tfm *tfm_io, *tfm_aes,
            *tfm_wait_progress, *tfm_wait_split,
            *tfm_write, *tfm_write_progress, *tfm_viz,
            *tfm_max_abs, *tfm_max_trace;

    struct render *render_progress, *render_split;

    struct viz_args viz_args = {
            .rows = 1,
            .cols = 1,
            .plots = 16,
            .samples = 1000,
            .rate = 1.0f,
            .fill_order = {PLOTS, ROWS, COLS}
    };

    tfm_io_correlation(&tfm_io, 16, 16);
    tfm_analyze_aes(&tfm_aes, false, AES128_R0_HW_SBOXOUT);
    tfm_wait_on(&tfm_wait_progress, PORT_CPA_SPLIT_PM_PROGRESS);
    tfm_wait_on(&tfm_wait_split, PORT_CPA_SPLIT_PM);

    tfm_save(&tfm_write, "/mnt/raid0/Data/test/aes");
    tfm_save(&tfm_write_progress, "/mnt/raid0/Data/test/progress");
    tfm_block_maxabs(&tfm_max_abs, PER_SAMPLE, 256);
    tfm_block_select(&tfm_max_trace, 256, BLOCK_MAXABS);
    tfm_visualize(&tfm_viz, &viz_args);

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce_aligned.trs");
    ts_transform(&broken, source, tfm_aes);
    ts_transform(&progress, broken, tfm_wait_progress);
    ts_transform(&split, broken, tfm_wait_split);

    ts_transform(&written, broken, tfm_write);
    ts_transform(&written_progress, progress, tfm_write_progress);
    ts_transform(&max_trace, written_progress, tfm_max_trace);
    ts_transform(&visualized, max_trace, tfm_viz);
    ts_transform(&written_split, split, tfm_write_progress);

    ts_create_cache(progress, 1ull * 1024 * 1024 * 1024, 8);

    ts_render_async(visualized, 1, &render_progress);
    ts_render_async(written_split, 1, &render_split);
    ts_render(written, 16);
    ts_render_join(render_progress);
    ts_render_join(render_split);

    ts_close(written_split);
    ts_close(written_progress);
    ts_close(written);
    ts_close(split);
    ts_close(progress);
    ts_close(broken);
    ts_close(source);

//    struct trace_set *source, *max_abs, *visualized;
//    struct tfm *tfm_max_abs, *tfm_max_trace, *tfm_viz;
//
//    struct viz_args viz_args = {
//            .rows = 1,
//            .cols = 1,
//            .plots = 16,
//            .samples = 1000,
//            .rate = 1.0f,
//            .fill_order = {PLOTS, ROWS, COLS}
//    };
//
//    tfm_block_maxabs(&tfm_max_abs, PER_SAMPLE, 256);
//    tfm_block_select(&tfm_max_trace, 256, BLOCK_MAXABS);
//    tfm_visualize(&tfm_viz, &viz_args);
//
//    ts_open(&source, "/mnt/raid0/Data/test/pm_last/progress_5.trs");
//    ts_transform(&max_abs, source, tfm_max_abs);
//    ts_transform(&visualized, max_abs, tfm_viz);
//
//    ts_create_cache(source, 1ull * 1024 * 1024 * 1024, 16);
//    ts_render(visualized, 1);
//
//    ts_close(visualized);
//    ts_close(max_abs);
//    ts_close(source);
}
