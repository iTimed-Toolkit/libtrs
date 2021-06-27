#include "crypto.h"

#include <openssl/aes.h>
#include <openssl/evp.h>

#include <string.h>

void add_key(uint8_t state[16], uint8_t key[16])
{
    int i;
    for (i = 0; i < 16; i++)
        state[i] = state[i] ^ key[i];
}

void sub_bytes(uint8_t state[16])
{
    int i;
    uint8_t val;

    for(i = 0; i < 16; i++)
    {
        val = state[i];
        state[i] = sbox[val >> 4u][val & 0xfu];
    }
}

void shift_rows(uint8_t state[16])
{
    uint8_t temp1, temp2;

    temp1 = state[0x1];
    state[0x1] = state[0x5];
    state[0x5] = state[0x9];
    state[0x9] = state[0xd];
    state[0xd] = temp1;

    temp1 = state[0x2];
    temp2 = state[0xe];
    state[0x2] = state[0xa];
    state[0xe] = state[0x6];
    state[0xa] = temp1;
    state[0x6] = temp2;

    temp1 = state[0x3];
    state[0x3] = state[0xf];
    state[0xf] = state[0xb];
    state[0xb] = state[0x7];
    state[0x7] = temp1;
}


void mix_cols(uint8_t state[16])
{
    uint8_t r0, r1, r2, r3;
    int i;

    for (i = 0; i < 4; i++)
    {
        r0 = state[4 * i];
        r1 = state[4 * i + 1];
        r2 = state[4 * i + 2];
        r3 = state[4 * i + 3];

        // no reason for the "+ 0" here but it makes the code look more lined up :)
        state[4 * i + 0] = mul2_lookup[r0] ^ mul3_lookup[r1] ^ r2 ^ r3;
        state[4 * i + 1] = r0 ^ mul2_lookup[r1] ^ mul3_lookup[r2] ^ r3;
        state[4 * i + 2] = r0 ^ r1 ^ mul2_lookup[r2] ^ mul3_lookup[r3];
        state[4 * i + 3] = mul3_lookup[r0] ^ r1 ^ r2 ^ mul2_lookup[r3];
    }
}

int encrypt_aes128(uint8_t *data, uint8_t *key, uint8_t *out)
{
    int olen;

    EVP_CIPHER_CTX *en_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(en_ctx);
    EVP_EncryptInit_ex(en_ctx, EVP_aes_128_ecb(), NULL, key, NULL);

    EVP_EncryptUpdate(en_ctx, out, &olen, data, 16);
    EVP_CIPHER_CTX_free(en_ctx);
    return 0;
}

bool verify_aes128(uint8_t *data)
{
    uint8_t enc[16];
    encrypt_aes128(&data[0], &data[32], enc);
    return memcmp(enc, &data[16], 16) == 0;
}