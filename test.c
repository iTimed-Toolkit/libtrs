#include "transform.h"
#include "libtrs.h"

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
    struct trace_set *source, *narrowed, *broken, *written;
    struct tfm *tfm_reduce,  *tfm_io, *tfm_write;

    tfm_io_correlation(&tfm_io, 32, 8);
    tfm_save(&tfm_write, "/tmp/io_test");

    ts_open(&source, "/mnt/raid0/Data/em/rand_50M_pos1_cpu2_arm_ce_aligned.trs");

    tfm_narrow(&tfm_reduce, 0, 5000000, 0, ts_num_samples(source));

    ts_transform(&narrowed, source, tfm_reduce);
    ts_transform(&broken, narrowed, tfm_io);
    ts_transform(&written, broken, tfm_write);

    ts_create_cache(narrowed, 1ull * 1024 * 1024 * 1024, 16);

    ts_render(written, 8);

    ts_close(source);
    ts_close(narrowed);
    ts_close(broken);
    ts_close(written);

//    int i, j;
//    struct trace_set *result;
//    struct trace *t;
//    float *s;
//
//    ts_open(&result, "/mnt/raid0/Data/test/io_test_2.trs");
//
//    for(i = 0; i < ts_num_traces(result); i++)
//    {
//        trace_get(result, &t, i, true);
//        trace_samples(t, &s);
//
//        for(j = 0; j < ts_num_samples(result); j++)
//            fprintf(stderr, "%.3f ", s[j]);
//        fprintf(stderr, "\n");
//    }
}
