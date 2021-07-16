#include "transform.h"
#include "__tfm_internal.h"
#include "__trace_internal.h"

#include "crypto.h"

#include <errno.h>
#include <string.h>

// for each of 10 rounds and 16 byte indices, evaluate 4 intermediate models
// also for each of 16 byte indices, evaluate input/output correlations
#define NUM_PMS         (16 * (10 * 4 + 1))
#define PMS_PER_THREAD  (NUM_PMS / 8)

static inline uint8_t hamming_weight(uint8_t n)
{
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);
    return n;
}

static inline uint8_t hamming_distance(uint8_t n0, uint8_t n1)
{
    return hamming_weight(n0 ^ n1);
}

typedef enum
{
    s_add_round_key,
    s_sub_bytes,
    s_shift_rows,
    s_mix_cols
} aes_state_t;

void expand_key(uint8_t key[16], uint8_t key_sched[176])
{
    int i, j, prev_key_base, key_base = 0;
    uint8_t val;
    memcpy(key_sched, key, 16);

    for(i = 1; i < 11; i++)
    {
        prev_key_base = key_base;
        key_base = 16 * i;

        for(j = 0; j < 3; j++)
        {
            val = key_sched[prev_key_base + 13 + j];
            key_sched[key_base + j] = sbox[val >> 4u][val & 0xfu];
        }

        val = key_sched[prev_key_base + 12];
        key_sched[key_base + 3] = sbox[val >> 4u][val & 0xfu];

        key_sched[key_base] ^= rc_lookup[i - 1];

        for(j = 0; j < 4; j++)
            key_sched[key_base + j] = key_sched[key_base + j] ^
                                      key_sched[prev_key_base + j];

        for(j = 4; j < 16; j++)
            key_sched[key_base + j] = key_sched[key_base + j - 4] ^
                                      key_sched[prev_key_base + j];
    }
}

int aes128_knownkey_models(uint8_t *data, int index, float *res)
{
    int i, round = 0;
    int byte_index = (index % 16);
    int byte_model = (index / 16);

    aes_state_t aes_state = s_add_round_key;
    uint8_t state[16], key[16], key_expanded[176];
    memcpy(state, &data[0], 16);
    memcpy(key, &data[32], 16);

    expand_key(key, key_expanded);
    for(i = 0; i < byte_model; i++)
    {
        switch(aes_state)
        {
            case s_add_round_key:
                add_key(state, &key_expanded[16 * round]);
                round++; aes_state = s_sub_bytes;
                break;

            case s_sub_bytes:
                sub_bytes(state);
                aes_state = s_shift_rows;
                break;

            case s_shift_rows:
                shift_rows(state);
                aes_state = (round == 10 ? s_add_round_key : s_mix_cols);
                break;

            case s_mix_cols:
                mix_cols(state);
                aes_state = s_add_round_key;
                break;

            default:
                err("Unrecognized AES state\n");
                return -EINVAL;
        }
    }

    *res = (float) hamming_weight(state[byte_index]);
    return 0;
}

int tfm_aes_knownkey_init(struct trace_set *ts, void *arg)
{
    if(!ts || arg)
    {
        err("Invalid trace set or argument\n");
        return -EINVAL;
    }

    ts->num_traces = NUM_PMS / PMS_PER_THREAD;
    ts->num_samples = ts->prev->num_samples * PMS_PER_THREAD;

    return 0;
}

int tfm_aes_knownkey_exit(struct trace_set *ts, void *arg)
{
    if(!ts || arg)
    {
        err("Invalid trace set or init arg\n");
        return -EINVAL;
    }

    return 0;
}

void tfm_aes_knownkey_progress_title(char *dst, int len, size_t index, int count)
{
    int byte_index = (index % 16);
    int byte_model = (index / 16);

    aes_state_t aes_state = s_add_round_key;
    char *aes_state_str = "?";
    int i, round = 0;

    if(byte_model == 0)
        snprintf(dst, len, "CPA HW(pt[%i]) (%i traces)", byte_index, count);
    else if(byte_model == 40)
        snprintf(dst, len, "CPA HW(ct[%i]) (%i traces)", byte_index, count);
    else
    {
        // Run through the state machine to find the correct intermediate
        for(i = 0; i < byte_model; i++)
        {
            switch(aes_state)
            {
                case s_add_round_key:
                    round++; aes_state = s_sub_bytes;
                    aes_state_str = "AddRoundKey";
                    break;

                case s_sub_bytes:
                    aes_state = s_shift_rows;
                    aes_state_str = "SubBytes";
                    break;

                case s_shift_rows:
                    aes_state = (round == 10 ? s_add_round_key : s_mix_cols);
                    aes_state_str = "ShiftRows";
                    break;

                case s_mix_cols:
                    aes_state = s_add_round_key;
                    aes_state_str = "MixCols";
                    break;

                default:
                    err("Unrecognized AES state\n");
                    return;
            }
        }

        snprintf(dst, len, "CPA HW(%s_out_%i[%i]) (%i traces)",
                 aes_state_str, round, byte_index, count);
    }
}

int tfm_aes_knownkey(struct tfm **tfm)
{
    struct cpa_args cpa_args = {
            .power_model = aes128_knownkey_models,
            .num_models = PMS_PER_THREAD,
            .consumer_init = tfm_aes_knownkey_init,
            .consumer_exit = tfm_aes_knownkey_exit,
            .progress_title = tfm_aes_knownkey_progress_title,
            .init_args = NULL
    };

    return tfm_cpa(tfm, &cpa_args);
}
