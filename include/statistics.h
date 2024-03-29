#ifndef LIBTRS_STATISTICS_H
#define LIBTRS_STATISTICS_H

/* accumulators */
struct accumulator;

#define ONEHOT(name) STAT ## name = (1 << (name))

/*
 * Note: if you add a new enum here, you MUST also update the
 * dependencies table in __stat_internal.h
 */
enum
{
    _AVG = 0,
    _DEV, _COV, _PEARSON,
    _MAX, _MIN, _MAXABS, _MINABS
};

typedef enum
{
    ONEHOT(_AVG), ONEHOT(_DEV), ONEHOT(_COV), ONEHOT(_PEARSON),
    ONEHOT(_MAX), ONEHOT(_MIN), ONEHOT(_MAXABS), ONEHOT(_MINABS)
} stat_t;

int stat_create_single(struct accumulator **, stat_t);
int stat_accumulate_single(struct accumulator *, float);
int stat_accumulate_single_many(struct accumulator *, float *, int);

int stat_create_single_array(struct accumulator **, stat_t, int);
int stat_accumulate_single_array(struct accumulator *, float *, int );
int stat_accumulate_single_array_many(struct accumulator *, float *, int, int);

int stat_create_dual(struct accumulator **, stat_t);
int stat_accumulate_dual(struct accumulator *, float , float );
int stat_accumulate_dual_many(struct accumulator *, float *, float *, int);

int stat_create_dual_array(struct accumulator **, stat_t, int, int);
int stat_accumulate_dual_array(struct accumulator *, float *, float *, int, int);
int stat_accumulate_dual_array_many(struct accumulator *, float *, float *, int, int, int);

int stat_create_pattern_match(struct accumulator **, float *, int, int);
int stat_pattern_match(struct accumulator *, float *, int, float **);

int stat_reset_accumulator(struct accumulator *);
int stat_free_accumulator(struct accumulator *);
int stat_get(struct accumulator *, stat_t, int, float *);
int stat_get_all(struct accumulator *, stat_t, float **);

#endif //LIBTRS_STATISTICS_H
