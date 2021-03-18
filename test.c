#include "transform.h"
#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

int main()
{
    struct trace_set *source, *broken, *written;
    struct tfm *tfm_break, *tfm_write;

    tfm_analyze_aes(&tfm_break, true, AES128_ROUND10_HW_SBOXOUT);
    tfm_save(&tfm_write, "/mnt/raid0/Data/test/aes");

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce_aligned.trs");
    ts_transform(&broken, source, tfm_break);
    ts_transform(&written, broken, tfm_write);

    ts_create_cache(source, 2ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(broken, 2ull * 1024 * 1024 * 1024, 16);
    ts_render(written, 64);

    ts_close(source);
    ts_close(broken);
    ts_close(written);
}
