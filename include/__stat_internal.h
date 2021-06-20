#ifndef LIBTRS___STAT_INTERNAL_H
#define LIBTRS___STAT_INTERNAL_H

#include "statistics.h"
#include <immintrin.h>

#define ACCUMULATOR(name)   \
    union {                 \
        float f;            \
        float *a;           \
    } (name)

// structs
struct accumulator
{
    enum
    {
        ACC_SINGLE,
        ACC_DUAL,
        ACC_SINGLE_ARRAY,
        ACC_DUAL_ARRAY
    } type;
    stat_t capabilities;

    int dim0, dim1;
    float count;

    ACCUMULATOR(m);
    ACCUMULATOR(s);
    ACCUMULATOR(cov);
    ACCUMULATOR(max);
    ACCUMULATOR(min);
    ACCUMULATOR(maxabs);
    ACCUMULATOR(minabs);

    int (*reset)(struct accumulator *);
    int (*free)(struct accumulator *);
    int (*get)(struct accumulator *, stat_t, int, float *);
    int (*get_all)(struct accumulator *, stat_t, float **);

#if USE_GPU
    void *gpu_vars;
#endif
};

#define IF_CAP(acc, stat) \
    if((acc)->capabilities & (stat))

#define IF_NOT_CAP(acc, stat) \
    if(~(acc)->capabilities & (stat))

#if USE_GPU
int __init_single_array_gpu(struct accumulator *acc, int num);
int __accumulate_single_array_gpu(struct accumulator *acc, float *val, int len);
int __sync_single_array_gpu(struct accumulator *acc);

int __init_dual_array_gpu(struct accumulator *acc, int num);
int __accumulate_dual_array_gpu(struct accumulator *acc, float *val, int len);
int __sync_dual_array_gpu(struct accumulator *acc);
#endif

// loop wrapper macros

#if __AVX2__ && !__AVX__
#error "Found AVX2 but not AVX"
#endif

#if __AVX512F__ && !__AVX2__
#error "Found AVX512 but not AVX2"
#endif

#if __AVX512F__
#define LOOP_HAVE_512(i, bound, ...)  \
    if((i) + 16 <= (bound))          \
    {                               \
        __VA_ARGS__;                \
        (i) += 16; continue;        \
    }

#define IF_HAVE_512(...) \
    __VA_ARGS__;

#else
#define LOOP_HAVE_512(i, bound, ...)
#define IF_HAVE_512(...)
#endif

#if __AVX2__
#define LOOP_HAVE_256(i, bound, ...)  \
    if((i) + 8 <= (bound))           \
    {                               \
        __VA_ARGS__;               \
        (i) += 8; continue;         \
    }

#define IF_HAVE_256(...) \
    __VA_ARGS__;
#else
#define LOOP_HAVE_256(i, bound, ...)
#define IF_HAVE_256(...)
#endif

#if __AVX__
#define LOOP_HAVE_128(i, bound, ...)  \
    if((i) + 4 <= (bound))           \
    {                               \
        __VA_ARGS__;                \
        (i) += 4; continue;         \
    }

#define IF_HAVE_128(...) \
    __VA_ARGS__;
#else
#define LOOP_HAVE_128(i, bound, ...)
#define IF_HAVE_128(...)
#endif

// AVX variables and functions

#define AVX512  512
#define AVX256  256
#define AVX128

#define __defer_avx_var(type, name) \
    name ## _ ## type

#define __defer_avx_func(type, name, ...) \
    _mm ## type ## _ ## name ( __VA_ARGS__ )

#define avx_var(type, name) \
    __defer_avx_var(type, name)

#define avx_loadu_ps(type, ptr) \
    __defer_avx_func(type, loadu_ps, ptr)

#define avx_storeu_ps(type, ptr, val) \
    __defer_avx_func(type, storeu_ps, ptr, val)

#define avx_add_ps(type, arg1, arg2) \
    __defer_avx_func(type, add_ps, arg1, arg2)

#define avx_sub_ps(type, arg1, arg2) \
    __defer_avx_func(type, sub_ps, arg1, arg2)

#define avx_mul_ps(type, arg1, arg2) \
    __defer_avx_func(type, mul_ps, arg1, arg2)

#define avx_div_ps(type, arg1, arg2) \
    __defer_avx_func(type, div_ps, arg1, arg2)

#define avx_setzero_ps(type) \
    __defer_avx_func(type, setzero_ps,)

#define avx_sqrt_ps(type, arg) \
    __defer_avx_func(type, sqrt_ps, arg)

#define avx_max_ps(type, arg1, arg2) \
    __defer_avx_func(type, max_ps, arg1, arg2)

#define avx_min_ps(type, arg1, arg2) \
    __defer_avx_func(type, min_ps, arg1, arg2)

// high-level components

#define init_m(type, m_ptr, val_ptr)                    \
    avx_storeu_ps(type, m_ptr,                          \
        avx_loadu_ps(type, val_ptr));

#define fetch_data(type, val_name, m_name, s_name,      \
                    val_ptr, m_ptr, s_ptr)              \
    avx_var(type, val_name) =                           \
        avx_loadu_ps(type, val_ptr);                    \
    avx_var(type, m_name) = avx_loadu_ps(type, m_ptr);  \
    avx_var(type, s_name) = avx_loadu_ps(type, s_ptr);

#define fetch_cov(type, cov_name, cov_ptr)              \
        avx_var(type, cov_name) =                       \
            avx_loadu_ps(type, cov_ptr);

#define store_cov(type, cov_name, cov_ptr)              \
    avx_storeu_ps(type, cov_ptr,                        \
        avx_var(type, cov_name ## _new));

#define calc_new_a(type,                                \
                    m_name, val_name, cnt_name)         \
    avx_var(type, m_name ## _new) =                     \
        avx_add_ps(type,                                \
            avx_var(type, m_name),                      \
            avx_div_ps(type,                            \
                    avx_sub_ps(type,                    \
                        avx_var(type, val_name),        \
                        avx_var(type, m_name)),         \
                    avx_var(type, cnt_name)))           \

#define calc_new_s(type, m_name, s_name, val_name)      \
    avx_var(type, s_name ## _new) =                     \
        avx_add_ps(type,                                \
            avx_var(type, s_name),                      \
            avx_mul_ps(type,                            \
                avx_sub_ps(type,                        \
                    avx_var(type, val_name),            \
                    avx_var(type, m_name)),             \
                avx_sub_ps(type,                        \
                    avx_var(type, val_name),            \
                    avx_var(type, m_name ## _new))));

#define calc_new_cov(type, cov_name,                    \
                        val0_name, m0_name,             \
                        val1_ptr, m1_ptr)               \
    avx_var(type, cov_name ## _new) =                   \
        avx_add_ps(type,                                \
            avx_var(type, cov_name),                    \
            avx_mul_ps(type,                            \
                avx_sub_ps(type,                        \
                    avx_var(type, val0_name),           \
                    avx_var(type, m0_name)),            \
                avx_sub_ps(type,                        \
                    avx_loadu_ps(type, val1_ptr),       \
                    avx_loadu_ps(type, m1_ptr))));

#define reduce_dev(type, s_ptr, d_ptr, cnt_name)        \
    avx_storeu_ps(type,                                 \
        d_ptr,                                          \
        avx_sqrt_ps(type,                               \
            avx_div_ps(type,                            \
                avx_loadu_ps(type, s_ptr),              \
                avx_var(type, cnt_name))))

#define reduce_cov(type, s_ptr, d_ptr, cnt_name)        \
    avx_storeu_ps(type,                                 \
        d_ptr,                                          \
        avx_div_ps(type,                                \
            avx_loadu_ps(type, s_ptr),                  \
            avx_var(type, cnt_name)))

#define reduce_pearson(type, cov_ptr,                   \
                        s0_ptr, dev1_name,              \
                        d_ptr, cnt_name)                \
    avx_storeu_ps(type,                                 \
        d_ptr,                                          \
        avx_div_ps(type,                                \
            avx_loadu_ps(type, cov_ptr),                \
            avx_mul_ps(type,                            \
                avx_var(type, cnt_name),                \
                avx_mul_ps(type,                        \
                    avx_sqrt_ps(type,                   \
                        avx_div_ps(type,                \
                            avx_loadu_ps(type, s0_ptr),  \
                            avx_var(type, cnt_name))),  \
                    avx_var(type, dev1_name)))))

// entire functions

#define accumulate(type, m_name, s_name,                \
                    val_name, cnt_name,                 \
                    m_ptr, s_ptr, val_ptr)              \
    fetch_data(type, val_name, m_name, s_name,          \
                val_ptr, m_ptr, s_ptr);                 \
    calc_new_a(type, m_name, val_name, cnt_name);       \
    calc_new_s(type, m_name, s_name, val_name);         \
    avx_storeu_ps(type, m_ptr,                          \
        avx_var(type, m_name ## _new));                 \
    avx_storeu_ps(type, s_ptr,                          \
        avx_var(type, s_name ## _new));

#define accumulate_cov(type, cov_name, cov_ptr,         \
                        val0_name, m0_name,             \
                        val1_ptr, m1_ptr)               \
    fetch_cov(type, cov_name, cov_ptr);                 \
    calc_new_cov(type, cov_name, val0_name, m0_name,    \
                    val1_ptr, m1_ptr)                   \
    store_cov(type, cov_name, cov_ptr)

#define accumulate_max(type, val_name, val_ptr,         \
                            max_name, max_ptr)          \
    avx_var(type, val_name) =                           \
        avx_loadu_ps(type, val_ptr);                    \
    avx_var(type, max_name) =                           \
        avx_loadu_ps(type, max_ptr);                    \
    avx_var(type, max_name) =                           \
        avx_max_ps(type, avx_var(type, max_name),       \
                    avx_var(type, val_name));           \
    avx_storeu_ps(type, max_ptr,                        \
                    avx_var(type, max_name));

#define accumulate_min(type, val_name, val_ptr,         \
                            min_name, min_ptr)          \
    avx_var(type, val_name) =                           \
        avx_loadu_ps(type, val_ptr);                    \
    avx_var(type, min_name) =                           \
        avx_loadu_ps(type, min_ptr);                    \
    avx_var(type, min_name) =                           \
        avx_min_ps(type, avx_var(type, min_name),       \
                    avx_var(type, val_name));           \
    avx_storeu_ps(type, min_ptr,                        \
                    avx_var(type, min_name));

#endif //LIBTRS___STAT_INTERNAL_H
