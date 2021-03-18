#include "transform.h"
#include "libtrace.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

extern void expand_key(uint8_t key[16], uint8_t key_sched[176], int n);

extern void aes128_ttable_encrypt_ecb(uint8_t *msg, uint8_t key_sched[176]);

int verify_data(char *path)
{
    int offset = 0;
    int ret, i, j;
    uint8_t *data, *data_actual;

    uint8_t msg[16], sched[176];

    struct trace_set *ts;
    struct trace *curr, *actual = NULL;

    ret = ts_open(&ts, path);
    if(ret < 0)
    {
        printf("Failed to open trace set\n");
        return ret;
    }

    ts_create_cache(ts, 1ull * 1024 * 1024 * 1024, 16);

    for(i = 0; i < ts_num_traces(ts); i++)
    {
//        if(i % 10000 == 0)
//            printf("Checking trace %i\n", i);

        ret = trace_get(ts, &curr, i, false);
        if(ret < 0)
        {
            printf("Failed to get trace at index %i\n", i);
            return ret;
        }

        ret = trace_data_all(curr, &data);
        if(ret < 0)
        {
            printf("Failed to get data for index %i\n", i);
        }

        for(j = 0; j < 16; j++)
        {
            if(data[16 + j] != 0x00)
                break;
        }

        if(j == 16)
        {
            printf("Found all-zero ciphertext at index %i\n", i);
            offset++;
        }

        memcpy(msg, &data[0], 16);

        expand_key(&data[32], sched, 11);
        aes128_ttable_encrypt_ecb(msg, sched);

        if(offset > 0)
        {
            ret = trace_get(ts, &actual, i + offset, false);
            if(ret < 0)
            {
                printf("Failed to get actual trace at index %i offset %i\n", i, offset);
                return ret;
            }

            ret = trace_data_all(actual, &data_actual);
            if(ret < 0)
            {
                printf("Failed to get actual data at index %i offset %i\n", i, offset);
                return ret;
            }
        }
        else
        {
            data_actual = data;
        }

        for(j = 0; j < 16; j++)
        {
            if(msg[j] != data_actual[16 + j])
            {
                printf("Error: mismatch for trace %i at index %i. Expected %02X, got %02X\n",
                       i, j, data_actual[16 + j], msg[j]);
            }
        }

        trace_free(curr);
        if(actual)
            trace_free(actual);

        actual = NULL;
    }

    return 0;
}


int main()
{
    struct trace_set *source, *broken, *written;
    struct tfm *tfm_break, *tfm_write;

    tfm_analyze_aes(&tfm_break, true, AES128_ROUND10_HW_SBOXOUT);
    tfm_save(&tfm_write, "/tmp/aes");

    ts_open(&source, "/mnt/usb/rand_50M_pos1_cpu2_arm_ce_aligned.trs");
    ts_transform(&broken, source, tfm_break);
    ts_transform(&written, broken, tfm_write);

    ts_create_cache(source, 4ull * 1024 * 1024 * 1024, 16);
    ts_create_cache(broken, 4ull * 1024 * 1024 * 1024, 16);
    ts_render(written, 32);

    ts_close(source);
    ts_close(broken);
    ts_close(written);
}
