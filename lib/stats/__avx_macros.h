#ifndef LIBTRS___AVX_MACROS_H
#define LIBTRS___AVX_MACROS_H

#include <immintrin.h>

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

// Different conventions for AVX512
#define _mm512_broadcast_ss(var)    \
    _mm512_broadcastss_ps(          \
        _mm_broadcast_ss(var))

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

#define avx_andnot_ps(type, arg1, arg2) \
    __defer_avx_func(type, andnot_ps, arg1, arg2)

#define avx_mul_ps(type, arg1, arg2) \
    __defer_avx_func(type, mul_ps, arg1, arg2)

#define avx_div_ps(type, arg1, arg2) \
    __defer_avx_func(type, div_ps, arg1, arg2)

#define avx_setzero_ps(type) \
    __defer_avx_func(type, setzero_ps,)

#define avx_broadcast_ss(type, val) \
    __defer_avx_func(type, broadcast_ss, val)

#define avx_sqrt_ps(type, arg) \
    __defer_avx_func(type, sqrt_ps, arg)

#define avx_max_ps(type, arg1, arg2) \
    __defer_avx_func(type, max_ps, arg1, arg2)

#define avx_min_ps(type, arg1, arg2) \
    __defer_avx_func(type, min_ps, arg1, arg2)

// high-level components

static const float __gbl_abs_mask = -0.0f;
#define avx_abs_ps(type, val_name)                      \
        avx_andnot_ps(type, val_name,                   \
            avx_broadcast_ss(type, &__gbl_abs_mask))      \

#define init_m(type, m_ptr, val_ptr)                    \
    avx_storeu_ps(type, m_ptr,                          \
        avx_loadu_ps(type, val_ptr));

#define init_abs(type, acc_ptr, val_ptr)                \
    avx_storeu_ps(type, acc_ptr,                        \
        avx_abs_ps(type, avx_loadu_ps(type, val_ptr)));

#define fetch_data(type, val_name, m_name, s_name, \
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

#define calc_new_a(type, \
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

#define calc_new_cov(type, cov_name, \
                        val0_name, m0_name, \
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

#define reduce_pearson(type, cov_ptr, \
                        s0_ptr, dev1_name, \
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

#define accumulate(type, m_name, s_name, \
                    val_name, cnt_name, \
                    m_ptr, s_ptr, val_ptr)              \
    fetch_data(type, val_name, m_name, s_name,          \
                val_ptr, m_ptr, s_ptr);                 \
    calc_new_a(type, m_name, val_name, cnt_name);       \
    calc_new_s(type, m_name, s_name, val_name);         \
    avx_storeu_ps(type, m_ptr,                          \
        avx_var(type, m_name ## _new));                 \
    avx_storeu_ps(type, s_ptr,                          \
        avx_var(type, s_name ## _new));

#define accumulate_cov(type, cov_name, cov_ptr, \
                        val0_name, m0_name, \
                        val1_ptr, m1_ptr)               \
    fetch_cov(type, cov_name, cov_ptr);                 \
    calc_new_cov(type, cov_name, val0_name, m0_name,    \
                    val1_ptr, m1_ptr)                   \
    store_cov(type, cov_name, cov_ptr)

#define accumulate_max(type, val_name, val_ptr, \
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

#define accumulate_min(type, val_name, val_ptr, \
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

#define accumulate_maxabs(type, val_name, val_ptr, \
                            maxabs_name, maxabs_ptr)    \
    avx_var(type, val_name) =                           \
        avx_loadu_ps(type, val_ptr);                    \
    avx_var(type, maxabs_name) =                        \
        avx_loadu_ps(type, maxabs_ptr);                 \
    avx_var(type, maxabs_name) =                        \
        avx_max_ps(type, avx_var(type, maxabs_name),    \
                    avx_abs_ps(type,                    \
                    avx_var(type, val_name)));          \
    avx_storeu_ps(type, maxabs_ptr,                     \
        avx_var(type, maxabs_name));

#define accumulate_minabs(type, val_name, val_ptr, \
                            minabs_name, minabs_ptr)    \
    avx_var(type, val_name) =                           \
        avx_loadu_ps(type, val_ptr);                    \
    avx_var(type, minabs_name) =                        \
        avx_loadu_ps(type, minabs_ptr);                 \
    avx_var(type, minabs_name) =                        \
        avx_min_ps(type, avx_var(type, minabs_name),    \
                    avx_abs_ps(type,                    \
                    avx_var(type, val_name)));          \
    avx_storeu_ps(type, minabs_ptr,                     \
        avx_var(type, minabs_name));


#endif //LIBTRS___AVX_MACROS_H
