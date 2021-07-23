#ifndef LIBTRS___STAT_INTERNAL_H
#define LIBTRS___STAT_INTERNAL_H

#include "statistics.h"
#include <stdint.h>
#include <stdbool.h>

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
        // General types
        ACC_SINGLE,
        ACC_DUAL,
        ACC_SINGLE_ARRAY,
        ACC_DUAL_ARRAY,

        // Special-purpose
        ACC_PATTERN_MATCH
    } type;
    stat_t capabilities;

    int dim0, dim1;
    float count;

    ACCUMULATOR(_AVG);
    ACCUMULATOR(_DEV);
    ACCUMULATOR(_COV);
    ACCUMULATOR(_MAX);
    ACCUMULATOR(_MIN);
    ACCUMULATOR(_MAXABS);
    ACCUMULATOR(_MINABS);

    bool transpose;
    int (*reset)(struct accumulator *);
    int (*free)(struct accumulator *);
    int (*get)(struct accumulator *, stat_t, int, float *);
    int (*get_all)(struct accumulator *, stat_t, float **);

#if USE_GPU
    void *gpu_vars;
#endif
};

#if USE_GPU
    #if defined(__cplusplus)
    extern "C" int gpu_init_dual_array(struct accumulator *, int, int);
    extern "C" int gpu_free_dual_array(struct accumulator *);
    extern "C" int gpu_accumulate_dual_array(struct accumulator *, float *, float *, int, int);
    extern "C" int gpu_sync_dual_array(struct accumulator *);

    extern "C" int gpu_pattern_preprocess(float *pattern, int pattern_len, float **out, float *var);
    extern "C" int gpu_pattern_free(float *pattern);
    extern "C" int gpu_pattern_match(float *data, int data_len, float *pattern, int pattern_len, float s_pattern, float **pearson);
    #else
    int gpu_init_dual_array(struct accumulator *, int, int);
    int gpu_free_dual_array(struct accumulator *);
    int gpu_accumulate_dual_array(struct accumulator *, float *, float *, int, int);
    int gpu_sync_dual_array(struct accumulator *);

    int gpu_pattern_preprocess(float *, int, float **, float *);
    int gpu_pattern_free(float *);
    int gpu_pattern_match(float *, int, float *, int , float, float **);
    #endif
#endif

/*
* This table tells us which statistics (entries) depend on which
* other statistics (indices).
*/
static const uint32_t dependencies[] = {
        STAT_DEV | STAT_COV | STAT_PEARSON, // _AVG
        STAT_AVG | STAT_COV | STAT_PEARSON, // _DEV
        STAT_AVG | STAT_DEV | STAT_COV | STAT_PEARSON, // _COV
        0, 0, 0, 0, 0 // _PEARSON, all _MAX / _MIN
};

#define ONEHOT_NODECL(stat) 1 << ((stat))

#define IF_CAP(acc, stat)       \
    if((acc)->capabilities & ((ONEHOT_NODECL(stat)) | dependencies[stat]))

// todo: figure out some way to make this work with dependency tables :/
#define IF_NOT_CAP(acc, stat)                   \
    if(!((acc)->capabilities & (((stat)))))

#define CAP_INIT_SCALAR(acc, cap, val) \
    IF_CAP(acc, cap) { (acc)->cap.f = val; }

#define CAP_INIT_ARRAY(acc, cap, num, fail) \
    IF_CAP(acc, cap) { (acc)->cap.a = calloc(num, sizeof(float)); \
    if(!(acc)->cap.a) {err("Failed to alloc " #acc " for stat " #cap "\n") \
        goto fail; }}

#define CAP_RESET_SCALAR(acc, cap, val) \
    IF_CAP(acc, cap) { (acc)->cap.f = val; }

#define CAP_RESET_ARRAY(acc, cap, val, num) \
    IF_CAP(acc, cap) { memset((acc)->cap.a, val, (num) * sizeof(float)); }

#define CAP_FREE_ARRAY(acc, cap) \
    IF_CAP(acc, cap) { free((acc)->cap.a); }

#endif //LIBTRS___STAT_INTERNAL_H
