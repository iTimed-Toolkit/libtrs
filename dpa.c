#include "dpa.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#define PATH(arg)           (arg)->ts_path
#define CALC_THRD(arg)      (arg)->n_thrd
#define N_PMS(arg)          (arg)->n_power_models

struct result_data
{
    sem_t lock;
    size_t num_hyp, num_samples, num_traces;
    size_t num_calculated;

    double **pm_sum, **pm_sq;
    double *tr_sum, *tr_sq;
    double **product;
};

struct thread_arg
{
    int index;
    struct dpa_args *dpa_arg;

    enum thread_cmd
    {
        // calculation
        CMD_CALC,

        // generic
        CMD_NOP,
        CMD_EXIT
    } cmd;

    enum thread_stat
    {
        STAT_READY = 0,
        STAT_RUNNING,
        STAT_DONE,
        STAT_FAILED
    } status;

    struct trace_set *set;
    struct result_data *local_res;
    size_t base, tpb, curr;
};

#define LINEAR(res, pi, ti)     ((res)->num_hyp * (ti) + (pi))

void __free_result_data(struct result_data *data,
                        struct dpa_args *dpa_arg)
{
    int i;

    sem_destroy(&data->lock);
    for(i = 0; i < N_PMS(dpa_arg); i++)
    {
        if(data->pm_sum && data->pm_sum[i])
            free(data->pm_sum[i]);

        if(data->pm_sq && data->pm_sq[i])
            free(data->pm_sq[i]);

        if(data->product && data->product[i])
            free(data->product[i]);
    }

    if(data->pm_sum)
        free(data->pm_sum);

    if(data->pm_sq)
        free(data->pm_sq);

    if(data->product)
        free(data->product);

    if(data->tr_sum)
        free(data->tr_sum);

    if(data->tr_sq)
        free(data->tr_sq);

    free(data);
}

int __init_result_data(struct result_data **data,
                       struct trace_set *set,
                       struct dpa_args *dpa_arg)
{
    int ret, i;
    struct result_data *res;
    res = calloc(1, sizeof(struct result_data));

    if(!res)
        return -ENOMEM;

    res->num_hyp = 256;
    res->num_samples = ts_num_samples(set);
    res->num_traces = ts_num_traces(set);
    res->num_calculated = 0;

    ret = sem_init(&res->lock, 0, 1);
    if(ret < 0)
    {
        free(res);
        return -errno;
    }

    res->pm_sum = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->pm_sum)
        goto __fail;

    res->pm_sq = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->pm_sq)
        goto __fail;

    res->product = calloc(N_PMS(dpa_arg), sizeof(double *));
    if(!res->product)
        goto __fail;

    for(i = 0; i < N_PMS(dpa_arg); i++)
    {
        res->pm_sum[i] = calloc(256, sizeof(double));
        if(!res->pm_sum[i])
            goto __fail;

        res->pm_sq[i] = calloc(256, sizeof(double));
        if(!res->pm_sq[i])
            goto __fail;

        res->product[i] = calloc(256 * ts_num_samples(set), sizeof(double));
        if(!res->product[i])
            goto __fail;
    }

    res->tr_sum = calloc(ts_num_samples(set), sizeof(double));
    if(!res->tr_sum)
        goto __fail;

    res->tr_sq = calloc(ts_num_samples(set), sizeof(double));
    if(!res->tr_sq)
        goto __fail;

    *data = res;
    return 0;

__fail:
    __free_result_data(res, dpa_arg);
    *data = NULL;
    return -ENOMEM;
}

size_t __result_data_size(struct trace_set *set, int n_pm)
{
    return sizeof(double) * (n_pm * 2 * 256 +
                             2 * ts_num_samples(set) +
                             2 * n_pm * 256 * ts_num_samples(set));
}

static const unsigned char sbox_inv[16][16] =
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

static const uint32_t const_t0[256] = {
        0xa56363c6, 0x847c7cf8, 0x997777ee, 0x8d7b7bf6, 0xdf2f2ff, 0xbd6b6bd6, 0xb16f6fde, 0x54c5c591,
        0x50303060, 0x3010102, 0xa96767ce, 0x7d2b2b56, 0x19fefee7, 0x62d7d7b5, 0xe6abab4d, 0x9a7676ec,
        0x45caca8f, 0x9d82821f, 0x40c9c989, 0x877d7dfa, 0x15fafaef, 0xeb5959b2, 0xc947478e, 0xbf0f0fb,
        0xecadad41, 0x67d4d4b3, 0xfda2a25f, 0xeaafaf45, 0xbf9c9c23, 0xf7a4a453, 0x967272e4, 0x5bc0c09b,
        0xc2b7b775, 0x1cfdfde1, 0xae93933d, 0x6a26264c, 0x5a36366c, 0x413f3f7e, 0x2f7f7f5, 0x4fcccc83,
        0x5c343468, 0xf4a5a551, 0x34e5e5d1, 0x8f1f1f9, 0x937171e2, 0x73d8d8ab, 0x53313162, 0x3f15152a,
        0xc040408, 0x52c7c795, 0x65232346, 0x5ec3c39d, 0x28181830, 0xa1969637, 0xf05050a, 0xb59a9a2f,
        0x907070e, 0x36121224, 0x9b80801b, 0x3de2e2df, 0x26ebebcd, 0x6927274e, 0xcdb2b27f, 0x9f7575ea,
        0x1b090912, 0x9e83831d, 0x742c2c58, 0x2e1a1a34, 0x2d1b1b36, 0xb26e6edc, 0xee5a5ab4, 0xfba0a05b,
        0xf65252a4, 0x4d3b3b76, 0x61d6d6b7, 0xceb3b37d, 0x7b292952, 0x3ee3e3dd, 0x712f2f5e, 0x97848413,
        0xf55353a6, 0x68d1d1b9, 0x0, 0x2cededc1, 0x60202040, 0x1ffcfce3, 0xc8b1b179, 0xed5b5bb6,
        0xbe6a6ad4, 0x46cbcb8d, 0xd9bebe67, 0x4b393972, 0xde4a4a94, 0xd44c4c98, 0xe85858b0, 0x4acfcf85,
        0x6bd0d0bb, 0x2aefefc5, 0xe5aaaa4f, 0x16fbfbed, 0xc5434386, 0xd74d4d9a, 0x55333366, 0x94858511,
        0xcf45458a, 0x10f9f9e9, 0x6020204, 0x817f7ffe, 0xf05050a0, 0x443c3c78, 0xba9f9f25, 0xe3a8a84b,
        0xf35151a2, 0xfea3a35d, 0xc0404080, 0x8a8f8f05, 0xad92923f, 0xbc9d9d21, 0x48383870, 0x4f5f5f1,
        0xdfbcbc63, 0xc1b6b677, 0x75dadaaf, 0x63212142, 0x30101020, 0x1affffe5, 0xef3f3fd, 0x6dd2d2bf,
        0x4ccdcd81, 0x140c0c18, 0x35131326, 0x2fececc3, 0xe15f5fbe, 0xa2979735, 0xcc444488, 0x3917172e,
        0x57c4c493, 0xf2a7a755, 0x827e7efc, 0x473d3d7a, 0xac6464c8, 0xe75d5dba, 0x2b191932, 0x957373e6,
        0xa06060c0, 0x98818119, 0xd14f4f9e, 0x7fdcdca3, 0x66222244, 0x7e2a2a54, 0xab90903b, 0x8388880b,
        0xca46468c, 0x29eeeec7, 0xd3b8b86b, 0x3c141428, 0x79dedea7, 0xe25e5ebc, 0x1d0b0b16, 0x76dbdbad,
        0x3be0e0db, 0x56323264, 0x4e3a3a74, 0x1e0a0a14, 0xdb494992, 0xa06060c, 0x6c242448, 0xe45c5cb8,
        0x5dc2c29f, 0x6ed3d3bd, 0xefacac43, 0xa66262c4, 0xa8919139, 0xa4959531, 0x37e4e4d3, 0x8b7979f2,
        0x32e7e7d5, 0x43c8c88b, 0x5937376e, 0xb76d6dda, 0x8c8d8d01, 0x64d5d5b1, 0xd24e4e9c, 0xe0a9a949,
        0xb46c6cd8, 0xfa5656ac, 0x7f4f4f3, 0x25eaeacf, 0xaf6565ca, 0x8e7a7af4, 0xe9aeae47, 0x18080810,
        0xd5baba6f, 0x887878f0, 0x6f25254a, 0x722e2e5c, 0x241c1c38, 0xf1a6a657, 0xc7b4b473, 0x51c6c697,
        0x23e8e8cb, 0x7cdddda1, 0x9c7474e8, 0x211f1f3e, 0xdd4b4b96, 0xdcbdbd61, 0x868b8b0d, 0x858a8a0f,
        0x907070e0, 0x423e3e7c, 0xc4b5b571, 0xaa6666cc, 0xd8484890, 0x5030306, 0x1f6f6f7, 0x120e0e1c,
        0xa36161c2, 0x5f35356a, 0xf95757ae, 0xd0b9b969, 0x91868617, 0x58c1c199, 0x271d1d3a, 0xb99e9e27,
        0x38e1e1d9, 0x13f8f8eb, 0xb398982b, 0x33111122, 0xbb6969d2, 0x70d9d9a9, 0x898e8e07, 0xa7949433,
        0xb69b9b2d, 0x221e1e3c, 0x92878715, 0x20e9e9c9, 0x49cece87, 0xff5555aa, 0x78282850, 0x7adfdfa5,
        0x8f8c8c03, 0xf8a1a159, 0x80898909, 0x170d0d1a, 0xdabfbf65, 0x31e6e6d7, 0xc6424284, 0xb86868d0,
        0xc3414182, 0xb0999929, 0x772d2d5a, 0x110f0f1e, 0xcbb0b07b, 0xfc5454a8, 0xd6bbbb6d, 0x3a16162c
};

static const uint32_t const_t1[256] = {
        0x6363c6a5, 0x7c7cf884, 0x7777ee99, 0x7b7bf68d, 0xf2f2ff0d, 0x6b6bd6bd, 0x6f6fdeb1, 0xc5c59154,
        0x30306050, 0x1010203, 0x6767cea9, 0x2b2b567d, 0xfefee719, 0xd7d7b562, 0xabab4de6, 0x7676ec9a,
        0xcaca8f45, 0x82821f9d, 0xc9c98940, 0x7d7dfa87, 0xfafaef15, 0x5959b2eb, 0x47478ec9, 0xf0f0fb0b,
        0xadad41ec, 0xd4d4b367, 0xa2a25ffd, 0xafaf45ea, 0x9c9c23bf, 0xa4a453f7, 0x7272e496, 0xc0c09b5b,
        0xb7b775c2, 0xfdfde11c, 0x93933dae, 0x26264c6a, 0x36366c5a, 0x3f3f7e41, 0xf7f7f502, 0xcccc834f,
        0x3434685c, 0xa5a551f4, 0xe5e5d134, 0xf1f1f908, 0x7171e293, 0xd8d8ab73, 0x31316253, 0x15152a3f,
        0x404080c, 0xc7c79552, 0x23234665, 0xc3c39d5e, 0x18183028, 0x969637a1, 0x5050a0f, 0x9a9a2fb5,
        0x7070e09, 0x12122436, 0x80801b9b, 0xe2e2df3d, 0xebebcd26, 0x27274e69, 0xb2b27fcd, 0x7575ea9f,
        0x909121b, 0x83831d9e, 0x2c2c5874, 0x1a1a342e, 0x1b1b362d, 0x6e6edcb2, 0x5a5ab4ee, 0xa0a05bfb,
        0x5252a4f6, 0x3b3b764d, 0xd6d6b761, 0xb3b37dce, 0x2929527b, 0xe3e3dd3e, 0x2f2f5e71, 0x84841397,
        0x5353a6f5, 0xd1d1b968, 0x0, 0xededc12c, 0x20204060, 0xfcfce31f, 0xb1b179c8, 0x5b5bb6ed,
        0x6a6ad4be, 0xcbcb8d46, 0xbebe67d9, 0x3939724b, 0x4a4a94de, 0x4c4c98d4, 0x5858b0e8, 0xcfcf854a,
        0xd0d0bb6b, 0xefefc52a, 0xaaaa4fe5, 0xfbfbed16, 0x434386c5, 0x4d4d9ad7, 0x33336655, 0x85851194,
        0x45458acf, 0xf9f9e910, 0x2020406, 0x7f7ffe81, 0x5050a0f0, 0x3c3c7844, 0x9f9f25ba, 0xa8a84be3,
        0x5151a2f3, 0xa3a35dfe, 0x404080c0, 0x8f8f058a, 0x92923fad, 0x9d9d21bc, 0x38387048, 0xf5f5f104,
        0xbcbc63df, 0xb6b677c1, 0xdadaaf75, 0x21214263, 0x10102030, 0xffffe51a, 0xf3f3fd0e, 0xd2d2bf6d,
        0xcdcd814c, 0xc0c1814, 0x13132635, 0xececc32f, 0x5f5fbee1, 0x979735a2, 0x444488cc, 0x17172e39,
        0xc4c49357, 0xa7a755f2, 0x7e7efc82, 0x3d3d7a47, 0x6464c8ac, 0x5d5dbae7, 0x1919322b, 0x7373e695,
        0x6060c0a0, 0x81811998, 0x4f4f9ed1, 0xdcdca37f, 0x22224466, 0x2a2a547e, 0x90903bab, 0x88880b83,
        0x46468cca, 0xeeeec729, 0xb8b86bd3, 0x1414283c, 0xdedea779, 0x5e5ebce2, 0xb0b161d, 0xdbdbad76,
        0xe0e0db3b, 0x32326456, 0x3a3a744e, 0xa0a141e, 0x494992db, 0x6060c0a, 0x2424486c, 0x5c5cb8e4,
        0xc2c29f5d, 0xd3d3bd6e, 0xacac43ef, 0x6262c4a6, 0x919139a8, 0x959531a4, 0xe4e4d337, 0x7979f28b,
        0xe7e7d532, 0xc8c88b43, 0x37376e59, 0x6d6ddab7, 0x8d8d018c, 0xd5d5b164, 0x4e4e9cd2, 0xa9a949e0,
        0x6c6cd8b4, 0x5656acfa, 0xf4f4f307, 0xeaeacf25, 0x6565caaf, 0x7a7af48e, 0xaeae47e9, 0x8081018,
        0xbaba6fd5, 0x7878f088, 0x25254a6f, 0x2e2e5c72, 0x1c1c3824, 0xa6a657f1, 0xb4b473c7, 0xc6c69751,
        0xe8e8cb23, 0xdddda17c, 0x7474e89c, 0x1f1f3e21, 0x4b4b96dd, 0xbdbd61dc, 0x8b8b0d86, 0x8a8a0f85,
        0x7070e090, 0x3e3e7c42, 0xb5b571c4, 0x6666ccaa, 0x484890d8, 0x3030605, 0xf6f6f701, 0xe0e1c12,
        0x6161c2a3, 0x35356a5f, 0x5757aef9, 0xb9b969d0, 0x86861791, 0xc1c19958, 0x1d1d3a27, 0x9e9e27b9,
        0xe1e1d938, 0xf8f8eb13, 0x98982bb3, 0x11112233, 0x6969d2bb, 0xd9d9a970, 0x8e8e0789, 0x949433a7,
        0x9b9b2db6, 0x1e1e3c22, 0x87871592, 0xe9e9c920, 0xcece8749, 0x5555aaff, 0x28285078, 0xdfdfa57a,
        0x8c8c038f, 0xa1a159f8, 0x89890980, 0xd0d1a17, 0xbfbf65da, 0xe6e6d731, 0x424284c6, 0x6868d0b8,
        0x414182c3, 0x999929b0, 0x2d2d5a77, 0xf0f1e11, 0xb0b07bcb, 0x5454a8fc, 0xbbbb6dd6, 0x16162c3a
};

static const uint32_t const_t2[256] = {
        0x63c6a563, 0x7cf8847c, 0x77ee9977, 0x7bf68d7b, 0xf2ff0df2, 0x6bd6bd6b, 0x6fdeb16f, 0xc59154c5,
        0x30605030, 0x1020301, 0x67cea967, 0x2b567d2b, 0xfee719fe, 0xd7b562d7, 0xab4de6ab, 0x76ec9a76,
        0xca8f45ca, 0x821f9d82, 0xc98940c9, 0x7dfa877d, 0xfaef15fa, 0x59b2eb59, 0x478ec947, 0xf0fb0bf0,
        0xad41ecad, 0xd4b367d4, 0xa25ffda2, 0xaf45eaaf, 0x9c23bf9c, 0xa453f7a4, 0x72e49672, 0xc09b5bc0,
        0xb775c2b7, 0xfde11cfd, 0x933dae93, 0x264c6a26, 0x366c5a36, 0x3f7e413f, 0xf7f502f7, 0xcc834fcc,
        0x34685c34, 0xa551f4a5, 0xe5d134e5, 0xf1f908f1, 0x71e29371, 0xd8ab73d8, 0x31625331, 0x152a3f15,
        0x4080c04, 0xc79552c7, 0x23466523, 0xc39d5ec3, 0x18302818, 0x9637a196, 0x50a0f05, 0x9a2fb59a,
        0x70e0907, 0x12243612, 0x801b9b80, 0xe2df3de2, 0xebcd26eb, 0x274e6927, 0xb27fcdb2, 0x75ea9f75,
        0x9121b09, 0x831d9e83, 0x2c58742c, 0x1a342e1a, 0x1b362d1b, 0x6edcb26e, 0x5ab4ee5a, 0xa05bfba0,
        0x52a4f652, 0x3b764d3b, 0xd6b761d6, 0xb37dceb3, 0x29527b29, 0xe3dd3ee3, 0x2f5e712f, 0x84139784,
        0x53a6f553, 0xd1b968d1, 0x0, 0xedc12ced, 0x20406020, 0xfce31ffc, 0xb179c8b1, 0x5bb6ed5b,
        0x6ad4be6a, 0xcb8d46cb, 0xbe67d9be, 0x39724b39, 0x4a94de4a, 0x4c98d44c, 0x58b0e858, 0xcf854acf,
        0xd0bb6bd0, 0xefc52aef, 0xaa4fe5aa, 0xfbed16fb, 0x4386c543, 0x4d9ad74d, 0x33665533, 0x85119485,
        0x458acf45, 0xf9e910f9, 0x2040602, 0x7ffe817f, 0x50a0f050, 0x3c78443c, 0x9f25ba9f, 0xa84be3a8,
        0x51a2f351, 0xa35dfea3, 0x4080c040, 0x8f058a8f, 0x923fad92, 0x9d21bc9d, 0x38704838, 0xf5f104f5,
        0xbc63dfbc, 0xb677c1b6, 0xdaaf75da, 0x21426321, 0x10203010, 0xffe51aff, 0xf3fd0ef3, 0xd2bf6dd2,
        0xcd814ccd, 0xc18140c, 0x13263513, 0xecc32fec, 0x5fbee15f, 0x9735a297, 0x4488cc44, 0x172e3917,
        0xc49357c4, 0xa755f2a7, 0x7efc827e, 0x3d7a473d, 0x64c8ac64, 0x5dbae75d, 0x19322b19, 0x73e69573,
        0x60c0a060, 0x81199881, 0x4f9ed14f, 0xdca37fdc, 0x22446622, 0x2a547e2a, 0x903bab90, 0x880b8388,
        0x468cca46, 0xeec729ee, 0xb86bd3b8, 0x14283c14, 0xdea779de, 0x5ebce25e, 0xb161d0b, 0xdbad76db,
        0xe0db3be0, 0x32645632, 0x3a744e3a, 0xa141e0a, 0x4992db49, 0x60c0a06, 0x24486c24, 0x5cb8e45c,
        0xc29f5dc2, 0xd3bd6ed3, 0xac43efac, 0x62c4a662, 0x9139a891, 0x9531a495, 0xe4d337e4, 0x79f28b79,
        0xe7d532e7, 0xc88b43c8, 0x376e5937, 0x6ddab76d, 0x8d018c8d, 0xd5b164d5, 0x4e9cd24e, 0xa949e0a9,
        0x6cd8b46c, 0x56acfa56, 0xf4f307f4, 0xeacf25ea, 0x65caaf65, 0x7af48e7a, 0xae47e9ae, 0x8101808,
        0xba6fd5ba, 0x78f08878, 0x254a6f25, 0x2e5c722e, 0x1c38241c, 0xa657f1a6, 0xb473c7b4, 0xc69751c6,
        0xe8cb23e8, 0xdda17cdd, 0x74e89c74, 0x1f3e211f, 0x4b96dd4b, 0xbd61dcbd, 0x8b0d868b, 0x8a0f858a,
        0x70e09070, 0x3e7c423e, 0xb571c4b5, 0x66ccaa66, 0x4890d848, 0x3060503, 0xf6f701f6, 0xe1c120e,
        0x61c2a361, 0x356a5f35, 0x57aef957, 0xb969d0b9, 0x86179186, 0xc19958c1, 0x1d3a271d, 0x9e27b99e,
        0xe1d938e1, 0xf8eb13f8, 0x982bb398, 0x11223311, 0x69d2bb69, 0xd9a970d9, 0x8e07898e, 0x9433a794,
        0x9b2db69b, 0x1e3c221e, 0x87159287, 0xe9c920e9, 0xce8749ce, 0x55aaff55, 0x28507828, 0xdfa57adf,
        0x8c038f8c, 0xa159f8a1, 0x89098089, 0xd1a170d, 0xbf65dabf, 0xe6d731e6, 0x4284c642, 0x68d0b868,
        0x4182c341, 0x9929b099, 0x2d5a772d, 0xf1e110f, 0xb07bcbb0, 0x54a8fc54, 0xbb6dd6bb, 0x162c3a16
};

static const uint32_t const_t3[256] = {
        0xc6a56363, 0xf8847c7c, 0xee997777, 0xf68d7b7b, 0xff0df2f2, 0xd6bd6b6b, 0xdeb16f6f, 0x9154c5c5,
        0x60503030, 0x2030101, 0xcea96767, 0x567d2b2b, 0xe719fefe, 0xb562d7d7, 0x4de6abab, 0xec9a7676,
        0x8f45caca, 0x1f9d8282, 0x8940c9c9, 0xfa877d7d, 0xef15fafa, 0xb2eb5959, 0x8ec94747, 0xfb0bf0f0,
        0x41ecadad, 0xb367d4d4, 0x5ffda2a2, 0x45eaafaf, 0x23bf9c9c, 0x53f7a4a4, 0xe4967272, 0x9b5bc0c0,
        0x75c2b7b7, 0xe11cfdfd, 0x3dae9393, 0x4c6a2626, 0x6c5a3636, 0x7e413f3f, 0xf502f7f7, 0x834fcccc,
        0x685c3434, 0x51f4a5a5, 0xd134e5e5, 0xf908f1f1, 0xe2937171, 0xab73d8d8, 0x62533131, 0x2a3f1515,
        0x80c0404, 0x9552c7c7, 0x46652323, 0x9d5ec3c3, 0x30281818, 0x37a19696, 0xa0f0505, 0x2fb59a9a,
        0xe090707, 0x24361212, 0x1b9b8080, 0xdf3de2e2, 0xcd26ebeb, 0x4e692727, 0x7fcdb2b2, 0xea9f7575,
        0x121b0909, 0x1d9e8383, 0x58742c2c, 0x342e1a1a, 0x362d1b1b, 0xdcb26e6e, 0xb4ee5a5a, 0x5bfba0a0,
        0xa4f65252, 0x764d3b3b, 0xb761d6d6, 0x7dceb3b3, 0x527b2929, 0xdd3ee3e3, 0x5e712f2f, 0x13978484,
        0xa6f55353, 0xb968d1d1, 0x0, 0xc12ceded, 0x40602020, 0xe31ffcfc, 0x79c8b1b1, 0xb6ed5b5b,
        0xd4be6a6a, 0x8d46cbcb, 0x67d9bebe, 0x724b3939, 0x94de4a4a, 0x98d44c4c, 0xb0e85858, 0x854acfcf,
        0xbb6bd0d0, 0xc52aefef, 0x4fe5aaaa, 0xed16fbfb, 0x86c54343, 0x9ad74d4d, 0x66553333, 0x11948585,
        0x8acf4545, 0xe910f9f9, 0x4060202, 0xfe817f7f, 0xa0f05050, 0x78443c3c, 0x25ba9f9f, 0x4be3a8a8,
        0xa2f35151, 0x5dfea3a3, 0x80c04040, 0x58a8f8f, 0x3fad9292, 0x21bc9d9d, 0x70483838, 0xf104f5f5,
        0x63dfbcbc, 0x77c1b6b6, 0xaf75dada, 0x42632121, 0x20301010, 0xe51affff, 0xfd0ef3f3, 0xbf6dd2d2,
        0x814ccdcd, 0x18140c0c, 0x26351313, 0xc32fecec, 0xbee15f5f, 0x35a29797, 0x88cc4444, 0x2e391717,
        0x9357c4c4, 0x55f2a7a7, 0xfc827e7e, 0x7a473d3d, 0xc8ac6464, 0xbae75d5d, 0x322b1919, 0xe6957373,
        0xc0a06060, 0x19988181, 0x9ed14f4f, 0xa37fdcdc, 0x44662222, 0x547e2a2a, 0x3bab9090, 0xb838888,
        0x8cca4646, 0xc729eeee, 0x6bd3b8b8, 0x283c1414, 0xa779dede, 0xbce25e5e, 0x161d0b0b, 0xad76dbdb,
        0xdb3be0e0, 0x64563232, 0x744e3a3a, 0x141e0a0a, 0x92db4949, 0xc0a0606, 0x486c2424, 0xb8e45c5c,
        0x9f5dc2c2, 0xbd6ed3d3, 0x43efacac, 0xc4a66262, 0x39a89191, 0x31a49595, 0xd337e4e4, 0xf28b7979,
        0xd532e7e7, 0x8b43c8c8, 0x6e593737, 0xdab76d6d, 0x18c8d8d, 0xb164d5d5, 0x9cd24e4e, 0x49e0a9a9,
        0xd8b46c6c, 0xacfa5656, 0xf307f4f4, 0xcf25eaea, 0xcaaf6565, 0xf48e7a7a, 0x47e9aeae, 0x10180808,
        0x6fd5baba, 0xf0887878, 0x4a6f2525, 0x5c722e2e, 0x38241c1c, 0x57f1a6a6, 0x73c7b4b4, 0x9751c6c6,
        0xcb23e8e8, 0xa17cdddd, 0xe89c7474, 0x3e211f1f, 0x96dd4b4b, 0x61dcbdbd, 0xd868b8b, 0xf858a8a,
        0xe0907070, 0x7c423e3e, 0x71c4b5b5, 0xccaa6666, 0x90d84848, 0x6050303, 0xf701f6f6, 0x1c120e0e,
        0xc2a36161, 0x6a5f3535, 0xaef95757, 0x69d0b9b9, 0x17918686, 0x9958c1c1, 0x3a271d1d, 0x27b99e9e,
        0xd938e1e1, 0xeb13f8f8, 0x2bb39898, 0x22331111, 0xd2bb6969, 0xa970d9d9, 0x7898e8e, 0x33a79494,
        0x2db69b9b, 0x3c221e1e, 0x15928787, 0xc920e9e9, 0x8749cece, 0xaaff5555, 0x50782828, 0xa57adfdf,
        0x38f8c8c, 0x59f8a1a1, 0x9808989, 0x1a170d0d, 0x65dabfbf, 0xd731e6e6, 0x84c64242, 0xd0b86868,
        0x82c34141, 0x29b09999, 0x5a772d2d, 0x1e110f0f, 0x7bcbb0b0, 0xa8fc5454, 0x6dd6bbbb, 0x2c3a1616
};

static const uint32_t *ttables[4] = {
    const_t0, const_t1, const_t2, const_t3
};

static inline int hamming_weight(unsigned char n)
{
    n = ((n & 0xAAu) >> 1u) + (n & 0x55u);
    n = ((n & 0xCCu) >> 2u) + (n & 0x33u);
    n = ((n & 0xF0u) >> 4u) + (n & 0x0Fu);

    return (char) n;
}

static inline int hamming_weight32(uint32_t n)
{
    return hamming_weight(n & 0xFF) + hamming_weight((n >> 8) & 0xFF) +
           hamming_weight((n >> 16) & 0xFF) + hamming_weight((n >> 24) & 0xFF);
}

static inline int hamming_distance(unsigned char n, unsigned char p)
{
    return hamming_weight(n ^ p);
}

static inline float power_model(struct trace *t, size_t guess, int target)
{
    uint8_t *data;
    trace_data_all(t, &data);

    return (float) hamming_weight32(ttables[target % 4][data[target] ^ guess]);
}

void *calc_thread_func(void *arg)
{
    struct thread_arg *my_arg = (struct thread_arg *) arg;
    struct trace *curr;
    size_t trace;

    int ret, k, i, p;
    float *trace_data, power_models[N_PMS(my_arg->dpa_arg)][256];
    struct timeval start, stop, diff;
    struct result_data *local_res = my_arg->local_res;

    gettimeofday(&start, NULL);
    for(trace = 0;
        trace < my_arg->tpb &&
        my_arg->base + trace < ts_num_traces(my_arg->set);
        trace++)
    {
        gettimeofday(&stop, NULL);
        timersub(&stop, &start, &diff);

        if(diff.tv_sec >= 1)
        {
            my_arg->curr = trace;
            gettimeofday(&start, NULL);
        }

        ret = trace_get(my_arg->set, &curr, my_arg->base + trace, false);
        if(ret < 0)
        {
            my_arg->status = ret;
            break;
        }

        ret = trace_samples(curr, &trace_data);
        if(ret < 0)
        {
            my_arg->status = ret;
            break;
        }

        sem_wait(&local_res->lock);

        local_res->num_calculated++;
        for(i = 0; i < ts_num_samples(my_arg->set); i++)
        {
            local_res->tr_sum[i] += trace_data[i];
            local_res->tr_sq[i] += (trace_data[i] * trace_data[i]);

            for(p = 0; p < N_PMS(my_arg->dpa_arg); p++)
            {
                for(k = 0; k < 256; k++)
                {
                    if(i == 0)
                    {
                        power_models[p][k] = power_model(curr, k, p);
                        local_res->pm_sum[p][k] += power_models[p][k];
                        local_res->pm_sq[p][k] += (power_models[p][k] * power_models[p][k]);
                    }

                    local_res->product[p][LINEAR(local_res, k, i)] +=
                            power_models[p][k] * trace_data[i];
                }
            }
        }
        sem_post(&local_res->lock);

        trace_free(curr);
    }

    my_arg->curr = trace;
    my_arg->status = STAT_DONE;
}

#define RESET_TIMERS(now, wait)             \
       gettimeofday(&(now), NULL);          \
       (wait).tv_sec = (now).tv_sec + 1;    \
       (wait).tv_nsec = 0

// todo: unglobal this
double *global_max_pearson, *global_max_k, *global_max_i;

#define CHECKPOINT_FREQ     1000
size_t last_checkpoint = 0;

void print_progress(struct thread_arg *t_args,
                    struct result_data *res,
                    struct dpa_args *dpa_arg)
{
    int i, j, k, p;
    double done;

    double pearson, pm_avg, pm_dev, tr_avg, tr_dev;
    double max_pearson, max_k, max_i, sig;

    char fname[256];
    FILE *checkpoint_file;

    if(!global_max_pearson)
        global_max_pearson = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_k)
        global_max_k = calloc(N_PMS(dpa_arg), sizeof(double));

    if(!global_max_i)
        global_max_i = calloc(N_PMS(dpa_arg), sizeof(double));

    for(j = 0; j < CALC_THRD(dpa_arg); j++)
    {
        sem_wait(&t_args[j].local_res->lock);

        if(j == 0)
        {
            res->num_calculated = t_args[j].local_res->num_calculated;
            for(i = 0; i < ts_num_samples(t_args[j].set); i++)
            {
                res->tr_sum[i] = t_args[j].local_res->tr_sum[i];
                res->tr_sq[i] = t_args[j].local_res->tr_sq[i];

                for(p = 0; p < N_PMS(dpa_arg); p++)
                {
                    for(k = 0; k < 256; k++)
                    {
                        if(i == 0)
                        {
                            res->pm_sum[p][k] = t_args[j].local_res->pm_sum[p][k];
                            res->pm_sq[p][k] = t_args[j].local_res->pm_sq[p][k];
                        }

                        res->product[p][LINEAR(res, k, i)] =
                                t_args[j].local_res->product[p][LINEAR(res, k, i)];
                    }
                }
            }
        }
        else
        {
            res->num_calculated += t_args[j].local_res->num_calculated;
            for(i = 0; i < ts_num_samples(t_args[j].set); i++)
            {
                res->tr_sum[i] += t_args[j].local_res->tr_sum[i];
                res->tr_sq[i] += t_args[j].local_res->tr_sq[i];

                for(p = 0; p < N_PMS(dpa_arg); p++)
                {
                    for(k = 0; k < 256; k++)
                    {
                        if(i == 0)
                        {
                            res->pm_sum[p][k] += t_args[j].local_res->pm_sum[p][k];
                            res->pm_sq[p][k] += t_args[j].local_res->pm_sq[p][k];
                        }

                        res->product[p][LINEAR(res, k, i)] +=
                                t_args[j].local_res->product[p][LINEAR(res, k, i)];
                    }
                }
            }
        }

        sem_post(&t_args[j].local_res->lock);
    }

    if(res->num_calculated >= last_checkpoint + CHECKPOINT_FREQ)
    {
        sprintf(fname, "checkpoint_%li.bin", res->num_calculated);
        checkpoint_file = fopen(fname, "wb+");

        fwrite("h", 1, 1, checkpoint_file);
        fwrite(&res->num_hyp, sizeof(size_t), 1, checkpoint_file);
        fwrite("s", 1, 1, checkpoint_file);
        fwrite(&res->num_samples, sizeof(size_t), 1, checkpoint_file);
        fwrite("t", 1, 1, checkpoint_file);
        fwrite(&res->num_traces, sizeof(size_t), 1, checkpoint_file);
        fwrite("p", 1, 1, checkpoint_file);
        fwrite(&dpa_arg->n_power_models, sizeof(size_t), 1, checkpoint_file);
        fwrite("c", 1, 1, checkpoint_file);
        fwrite(&res->num_calculated, sizeof(size_t), 1, checkpoint_file);

        fwrite("iu", 1, 2, checkpoint_file);
        fwrite(res->tr_sum, sizeof(double), res->num_samples, checkpoint_file);

        fwrite("is", 1, 2, checkpoint_file);
        fwrite(res->tr_sq, sizeof(double), res->num_samples, checkpoint_file);

        for(p = 0; p < N_PMS(dpa_arg); p++)
        {
            fwrite("ku", 1, 2, checkpoint_file);
            fwrite(res->pm_sum[p], sizeof(double), 256, checkpoint_file);
            fwrite("ks", 1, 2, checkpoint_file);
            fwrite(res->pm_sq[p], sizeof(double), 256, checkpoint_file);
            fwrite("ki", 1, 2, checkpoint_file);
            fwrite(res->product[p], sizeof(double), 256 * res->num_samples, checkpoint_file);
        }

        fflush(checkpoint_file);
        fclose(checkpoint_file);
        last_checkpoint = res->num_traces;
    }

    for(p = 0; p < N_PMS(dpa_arg); p++)
    {
        max_pearson = 0;

        for(k = 0; k < 256; k++)
        {
            for(i = 0; i < res->num_samples; i++)
            {
                pm_avg = res->pm_sum[p][k] / res->num_calculated;
                pm_dev = res->pm_sq[p][k] / res->num_calculated;
                pm_dev -= (pm_avg * pm_avg);
                pm_dev = sqrt(pm_dev);

                tr_avg = res->tr_sum[i] / res->num_calculated;
                tr_dev = res->tr_sq[i] / res->num_calculated;
                tr_dev -= (tr_avg * tr_avg);
                tr_dev = sqrt(tr_dev);

                pearson = res->product[p][LINEAR(res, k, i)];
                pearson -= res->tr_sum[i] * pm_avg;
                pearson -= res->pm_sum[p][k] * tr_avg;
                pearson += res->num_calculated * pm_avg * tr_avg;
                pearson /= (res->num_calculated * pm_dev * tr_dev);

                if(fabs(pearson) > max_pearson)
                {
                    max_pearson = fabs(pearson);
                    max_k = k;
                    max_i = i;
                }
            }
        }

        global_max_pearson[p] = max_pearson;
        global_max_k[p] = max_k;
        global_max_i[p] = max_i;
    }

    printf("\nCompute threads\n");
    for(i = 0; i < CALC_THRD(dpa_arg); i++)
    {
        done = t_args[i].tpb == 0 ?
               0 :
               ((double) t_args[i].curr /
                (double) t_args[i].tpb);

        printf("calc%i [", i);
        for(j = 0; j < 80; j++)
        {
            if((double) j / 80.0 < done)
                printf("=");
            else
                printf(" ");
        }
        printf("] %.2f%% (%li)\n", done * 100, t_args[i].base);
    }

    sig = 4.0 / sqrt((double) res->num_calculated);
    printf("\nCurrent Pearson (%li traces, sig = %f)\n", res->num_calculated, sig);
    for(p = 0; p < N_PMS(dpa_arg); p++)
    {
        printf("\t%i %f for guess 0x%02X at sample %i ",
               p, global_max_pearson[p], (uint8_t) global_max_k[p], (int) global_max_i[p]);

        if(global_max_pearson[p] > sig)
            printf("(!)");
        printf("\n");
    }

    printf("\n\n\n\n");
}

bool __any_running(struct thread_arg *calc_args,
                   struct dpa_args *dpa_args)
{
    int i;
    for(i = 0; i < CALC_THRD(dpa_args); i++)
    {
        if(calc_args[i].status == STAT_RUNNING)
            return true;
    }

    return false;
}

int run_dpa(struct dpa_args *arg)
{
    int i, j, k, ret;
    size_t batch = 0, trace_per_thread;

    struct trace_set *set;
    struct result_data *res;

    pthread_t t_handles[CALC_THRD(arg)];
    struct thread_arg t_args[CALC_THRD(arg)];

    ret = ts_open(&set, PATH(arg));
    if(ret < 0)
    {
        printf("couldn't create trace set: %s\n", strerror(-ret));
        return ret;
    }

    trace_per_thread = ts_num_traces(set) / CALC_THRD(arg);

    if(__init_result_data(&res, set, arg) < 0)
    {
        printf("couldn't init result data\n");
        goto __close_set;
    }

    for(i = 0; i < CALC_THRD(arg); i++)
    {
        // create calculation threads
        t_args[i].index = i;
        t_args[i].dpa_arg = arg;
        t_args[i].status = STAT_RUNNING;
        t_args[i].set = set;

        __init_result_data(&t_args[i].local_res, set, arg);
        t_args[i].base = i * trace_per_thread;
        t_args[i].tpb = trace_per_thread;
        t_args[i].curr = 0;

        ret = pthread_create(&t_handles[i], NULL,
                             calc_thread_func, &t_args[i]);
        if(ret < 0)
        {
            ret = -errno;
            goto __free_calc_args;
        }
    }

    while(__any_running(t_args, arg))
    {
        usleep(1000000);
        print_progress(t_args, res, arg);
    }

    print_progress(t_args, res, arg);

    i = CALC_THRD(arg) - 1;
__free_calc_args:
    for(j = 0; j <= i; j++)
    {
        // todo figure out some cancellation mechanism
        pthread_join(t_handles[i], NULL);
//        sem_destroy(&t_args[i].start);
    }

    if(res)
        __free_result_data(res, arg);

__close_set:
    ts_close(set);
    return ret;
}
