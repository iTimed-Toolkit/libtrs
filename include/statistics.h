#ifndef LIBTRS_STATISTICS_H
#define LIBTRS_STATISTICS_H

/* accumulators */
struct accumulator;

int stat_create_single(struct accumulator **acc);
int stat_create_single_array(struct accumulator **acc, int num);
int stat_create_dual(struct accumulator **acc);
int stat_create_dual_array(struct accumulator **acc, int num0, int num1);

int stat_reset_accumulator(struct accumulator *acc);
int stat_free_accumulator(struct accumulator *acc);

int stat_accumulate_single(struct accumulator *acc, float val);
int stat_accumulate_single_array(struct accumulator *acc, float *val, int len);
int stat_accumulate_dual(struct accumulator *acc, float val0, float val1);
int stat_accumulate_dual_array(struct accumulator *acc, float *val0, float *val1, int len0, int len1);

int stat_accumulate_single_many(struct accumulator *acc, float *val, int num);
int stat_accumulate_single_array_many(struct accumulator *acc, float *val, int len, int num);
int stat_accumulate_dual_many(struct accumulator *acc, float *val0, float *val1, int num);
int stat_accumulate_dual_array_many(struct accumulator *acc, float *val0, float *val1, int len0, int len1, int num);

int stat_get_mean(struct accumulator *acc, int index, float *res);
int stat_get_dev(struct accumulator *acc, int index, float *res);
int stat_get_cov(struct accumulator *acc, int index, float *res);
int stat_get_pearson(struct accumulator *acc, int index, float *res);

int stat_get_mean_all(struct accumulator *acc, float **res);
int stat_get_dev_all(struct accumulator *acc, float **res);
int stat_get_cov_all(struct accumulator *acc, float **res);
int stat_get_pearson_all(struct accumulator *acc, float **res);

/* oneshot */
int stat_oneshot_max(float *max, float *val, int len);
int stat_oneshot_min(float *min, float *val, int len);

#if defined(__cplusplus)

extern "C" int gpu_pattern_preprocess(float *pattern, int pattern_len, float **out, float *var);
extern "C" int gpu_pattern_free(float *pattern);
extern "C" int gpu_pattern_match(float *data, int data_len, float *pattern, int pattern_len, float s_pattern, float **pearson);

#else

int gpu_pattern_preprocess(float *pattern, int pattern_len, float **out, float *var);
int gpu_pattern_free(float *pattern);
int gpu_pattern_match(float *data, int data_len, float *pattern, int pattern_len, float s_pattern, float **pearson);

#endif

#endif //LIBTRS_STATISTICS_H
