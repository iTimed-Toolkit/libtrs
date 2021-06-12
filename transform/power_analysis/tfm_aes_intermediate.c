#include "transform.h"
#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "crypto.h"

#include <errno.h>
#include <string.h>

#define PMS_PER_THREAD  256

static inline uint8_t hamming_weight(uint8_t n)
{
    return __builtin_popcount(n);
//    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
//    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
//    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);
//    return n;
}

static inline uint8_t hamming_distance(uint8_t n0, uint8_t n1)
{
    return hamming_weight(n0 ^ n1);
}

int aes128_round10_hw_sbox_in(uint8_t *data, int index, float *res)
{
    int key_index = (index / 256);
    uint8_t key_guess = (index % 256), state;

    state = data[16 + shift_rows_inv_indices[key_index]] ^ key_guess;

    *res = (float) hamming_weight(sbox_inv[state >> 4u][state & 0xFu]);
    return 0;
}

int aes128_round10_hw_sbox_in_verify(uint8_t *data, int index, float *res)
{
    if(!verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round10_hw_sbox_in(data, index, res);
}

int aes128_round10_out_hd(uint8_t *data, int index, float *res)
{
    int key_index = (index / 256);
    uint8_t key_guess = (index % 256), state;

    state = data[16 + key_index] ^ key_guess;
    state = sbox_inv[state >> 4u][state & 0xFu];

    *res = (float) hamming_distance(state, data[16 + shift_rows_indices[key_index]]);
    return 0;
}


int aes128_round10_out_hd_verify(uint8_t *data, int index, float *res)
{
    if(!verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round10_out_hd(data, index, res);
}

int aes128_round0_hw_sbox_out(uint8_t *data, int index, float *res)
{
    int key_index = (index / 256);
    uint8_t key_guess = (index % 256), state;

    state = data[key_index] ^ key_guess;
    state = sbox[state >> 4u][state & 0xfu];
    *res = (float) hamming_weight(state);
    return 0;
}

int aes128_round0_hw_sbox_out_verify(uint8_t *data, int index, float *res)
{
    if(!verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round0_hw_sbox_out(data, index, res);
}

int aes128_round0_round1_hd(uint8_t *data, int index, float *res)
{
    int key_index = (index / 256);
    uint8_t key_guess = (index % 256), state;

    state = data[key_index] ^ key_guess;
    state = sbox[state >> 4u][state & 0xfu];
    *res = (float) hamming_distance(data[shift_rows_inv_indices[key_index]], state);

    return 0;
}

int aes128_round0_round1_hd_verify(uint8_t *data, int index, float *res)
{
    if(!verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round0_round1_hd(data, index, res);
}

struct tfm_aes_intermediate_arg
{
    aes_leakage_t leakage_model;
};

int tfm_aes_intermediate_init(struct trace_set *ts, void *arg)
{
    struct tfm_aes_intermediate_arg *aes_arg = arg;

    if(!ts || !arg)
    {
        err("Invalid trace set or argument\n");
        return -EINVAL;
    }

    switch(aes_arg->leakage_model)
    {
        case AES128_R0_R1_HD_NOMC:
        case AES128_R0_HW_SBOXOUT:
        case AES128_R10_OUT_HD:
        case AES128_R10_HW_SBOXIN:
            ts->num_traces = 16 * 256 / PMS_PER_THREAD;
            ts->num_samples = ts->prev->num_samples * PMS_PER_THREAD;
            break;

        default:
            err("Unrecognized leakage model\n");
            return -EINVAL;
    }

    return 0;
}

int tfm_aes_intermediate_exit(struct trace_set *ts, void *arg)
{
    if(!ts || !arg)
    {
        err("Invalid trace set or init arg\n");
        return -EINVAL;
    }

    free(arg);
    return 0;
}

void tfm_aes_intermediate_progress_title(char *dst, int len, size_t index, int count)
{
    size_t key_index = (index / 256);
    uint8_t key_guess = (index % 256);

    snprintf(dst, len, "CPA %li pm %02X (%i traces)", key_index, key_guess, count);
}

int tfm_aes_intermediate(struct tfm **tfm, bool verify_data, aes_leakage_t leakage_model)
{
    int ret;
    struct tfm_aes_intermediate_arg *arg;
    int (*model)(uint8_t *, int, float *);

    struct cpa_args cpa_args = {
            .power_model = NULL,
            .num_models = PMS_PER_THREAD,
            .consumer_init = tfm_aes_intermediate_init,
            .consumer_exit = tfm_aes_intermediate_exit,
            .progress_title = tfm_aes_intermediate_progress_title,
            .init_args = NULL
    };

    switch(leakage_model)
    {
        case AES128_R0_R1_HD_NOMC:
            if(verify_data) model = aes128_round0_round1_hd_verify;
            else model = aes128_round0_round1_hd;
            break;

        case AES128_R0_HW_SBOXOUT:
            if(verify_data) model = aes128_round0_hw_sbox_out_verify;
            else model = aes128_round0_hw_sbox_out;
            break;

        case AES128_R10_OUT_HD:
            if(verify_data) model = aes128_round10_out_hd_verify;
            else model = aes128_round10_out_hd;
            break;

        case AES128_R10_HW_SBOXIN:
            if(verify_data) model = aes128_round10_hw_sbox_in_verify;
            else model = aes128_round10_hw_sbox_in;
            break;

        default:
            err("Unrecognized leakage model\n");
            return -EINVAL;
    }

    arg = calloc(1, sizeof(struct tfm_aes_intermediate_arg));
    if(!arg)
    {
        err("Failed to allocate AES analyze arg\n");
        return -ENOMEM;
    }

    arg->leakage_model = leakage_model;

    cpa_args.power_model = model;
    cpa_args.init_args = arg;
    ret = tfm_cpa(tfm, &cpa_args);

    if(ret < 0)
    {
        err("Failed to initialize generic CPA transform\n");
        free(arg);
        return ret;
    }

    return 0;
}
