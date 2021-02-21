#ifndef LIBTRS_TRANSFORM_H
#define LIBTRS_TRANSFORM_H

#include <stdbool.h>

int tfm_average(void **tfm, bool per_sample);
int tfm_split_tvla(void **tfm, bool which);

#endif //LIBTRS_TRANSFORM_H
