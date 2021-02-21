#ifndef LIBTRS_TRANSFORM_H
#define LIBTRS_TRANSFORM_H

#include <stdbool.h>

struct tfm;

// Analysis
int tfm_average(struct tfm **tfm, bool per_sample);

// Traces
int tfm_split_tvla(struct tfm **tfm, bool which);

// Align

#endif //LIBTRS_TRANSFORM_H
