#include "transform.h"
#include "trace.h"

int main()
{
    struct trace_set *source, *broken, *written;
    struct tfm *tfm_break, *tfm_write;

    tfm_analyze_aes(&tfm_break, true, AES128_R10_HW_SBOXIN);
    tfm_save(&tfm_write, "/mnt/raid0/Data/test/aes2");

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce.trs");
    ts_transform(&broken, source, tfm_break);
    ts_transform(&written, broken, tfm_write);

    ts_create_cache(source, 1ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(broken, 1ull * 1024 * 1024 * 1024, 16);

    ts_render(written, 8);

    ts_close(written);
    ts_close(broken);
    ts_close(source);

//    for(i = 0; i < 10000; i++)
//    {
//        trace_get(aligned, &out, i, true);
//        trace_free(out);
//    }
}
