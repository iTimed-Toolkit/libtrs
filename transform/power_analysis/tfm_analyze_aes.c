#include "transform.h"
#include "__tfm_internal.h"

#include "__trace_internal.h"

#include <errno.h>
#include <string.h>

#include <openssl/aes.h>
#include <openssl/evp.h>

#define PMS_PER_THREAD  256

static const unsigned char sbox[16][16] =
        {
                {0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76},
                {0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0},
                {0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15},
                {0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75},
                {0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84},
                {0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf},
                {0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8},
                {0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2},
                {0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73},
                {0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb},
                {0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79},
                {0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08},
                {0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a},
                {0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e},
                {0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf},
                {0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16}
        };

static const uint8_t sbox_inv[16][16] =
        {
                {0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb},
                {0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb},
                {0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e},
                {0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25},
                {0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92},
                {0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84},
                {0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06},
                {0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b},
                {0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73},
                {0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e},
                {0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b},
                {0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4},
                {0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f},
                {0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef},
                {0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61},
                {0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d}
        };

static const uint8_t shift_rows[16] = {
        [0] = 0x0, [1] = 0x5, [2] = 0xA, [3] = 0xF,
        [4] = 0x4, [5] = 0x9, [6] = 0xE, [7] = 0x3,
        [8] = 0x8, [9] = 0xD, [10] = 0x2, [11] = 0x7,
        [12] = 0xC, [13] = 0x1, [14] = 0x6, [15] = 0xB
};

static const uint8_t shift_rows_inv[16] = {
        [0] = 0x0, [1] = 0xD, [2] = 0xA, [3] = 0x7,
        [4] = 0x4, [5] = 0x1, [6] = 0xE, [7] = 0xB,
        [8] = 0x8, [9] = 0x5, [10] = 0x2, [11] = 0xF,
        [12] = 0xC, [13] = 0x9, [14] = 0x6, [15] = 0x3
};

bool __verify_aes128(uint8_t *data)
{
    int olen;
    uint8_t enc[16];

    EVP_CIPHER_CTX *en_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(en_ctx);
    EVP_EncryptInit_ex(en_ctx, EVP_aes_128_ecb(), NULL, &data[32], NULL);

    EVP_EncryptUpdate(en_ctx, enc, &olen, &data[0], 16);
//    EVP_EncryptFinal_ex(en_ctx, enc + olen, &olen);
    EVP_CIPHER_CTX_free(en_ctx);

    return memcmp(enc, &data[16], 16) == 0;
}

static inline uint8_t hamming_weight(uint8_t n)
{
//    return __builtin_popcount(n);
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);
    return n;
}

static inline uint8_t hamming_distance(uint8_t n0, uint8_t n1)
{
    return hamming_weight(n0 ^ n1);
}

int aes128_round10_hw_sbox_in(uint8_t *data, int index, float *res)
{
    int key_index = (index / 256);
    uint8_t key_guess = (index % 256), state;

    state = data[16 + shift_rows_inv[key_index]] ^ key_guess;

    *res = (float) hamming_weight(sbox_inv[state >> 4u][state & 0xFu]);
    return 0;
}

int aes128_round10_hw_sbox_in_verify(uint8_t *data, int index, float *res)
{
    if(!__verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round10_hw_sbox_in(data, index, res);
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
    if(!__verify_aes128(data))
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
    *res = (float) hamming_distance(data[shift_rows_inv[key_index]], state);

//    if(key_index == 1)
//    {
//        critical("Key index %i, key guess %02X\n", key_index, key_guess);
//        critical("AddRoundKey: %02X ^ %02X = %02X\n", data[key_index], key_guess, data[key_index] ^ key_guess);
//        critical("SubBytes: %02X -> %02X\n", data[key_index] ^ key_guess, state);
//        critical("Hamming distance between %02X and m[%i] = %02X is %02X\n", state,
//                 shift_rows_inv[key_index], data[shift_rows_inv[key_index]],
//                 hamming_distance(data[shift_rows_inv[key_index]], state));
//    }

    return 0;
}

int aes128_round0_round1_hd_verify(uint8_t *data, int index, float *res)
{
    if(!__verify_aes128(data))
    {
        err("Data failed validation\n");
        return -EINVAL;
    }

    return aes128_round0_round1_hd(data, index, res);
}

struct tfm_analyze_aes_arg
{
    aes_leakage_t leakage_model;
};

int tfm_analyze_aes_init(struct trace_set *ts, void *arg)
{
    struct tfm_analyze_aes_arg *aes_arg = arg;

    if(!ts || !arg)
    {
        err("Invalid trace set or argument\n");
        return -EINVAL;
    }

    switch(aes_arg->leakage_model)
    {
        case AES128_R0_R1_HD_NOMC:
        case AES128_R0_HW_SBOXOUT:
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

int tfm_analyze_aes_exit(struct trace_set *ts, void *arg)
{
    if(!ts || !arg)
    {
        err("Invalid trace set or init arg\n");
        return -EINVAL;
    }

    free(arg);
    return 0;
}

void tfm_analyze_aes_progress_title(char *dst, int len, size_t index, int count)
{
    size_t key_index = (index / 256);
    uint8_t key_guess = (index % 256);

    snprintf(dst, len, "CPA %li pm %02X (%i traces)", key_index, key_guess, count);
}

int tfm_analyze_aes(struct tfm **tfm, bool verify_data, aes_leakage_t leakage_model)
{
    int ret;
    struct tfm_analyze_aes_arg *arg;
    int (*model)(uint8_t *, int, float *);

    struct cpa_args cpa_args = {
            .power_model = NULL,
            .num_models = PMS_PER_THREAD,
            .consumer_init = tfm_analyze_aes_init,
            .consumer_exit = tfm_analyze_aes_exit,
            .progress_title = tfm_analyze_aes_progress_title,
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

        case AES128_R10_HW_SBOXIN:
            if(verify_data) model = aes128_round10_hw_sbox_in_verify;
            else model = aes128_round10_hw_sbox_in;
            break;

        default:
            err("Unrecognized leakage model\n");
            return -EINVAL;
    }

    arg = calloc(1, sizeof(struct tfm_analyze_aes_arg));
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