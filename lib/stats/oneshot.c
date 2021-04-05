#include "statistics.h"
#include "__stat_internal.h"

#include "__trace_internal.h"

#include <errno.h>

int stat_oneshot_max(float *max, float *val, int len)
{
    int i;

    IF_HAVE_512(__m512 curr_512, max_512);
    IF_HAVE_256(__m256 curr_256, max_256);
    IF_HAVE_128(__m128 curr_, max_);

    if(!max || !val || len == 0)
    {
        err("Invalid args\n");
        return -EINVAL;
    }

    for(i = 0; i < len;)
    {
        LOOP_HAVE_512(i, len,
                      accumulate_max(AVX512,
                                     curr, &val[i],
                                     max, &max[i]));

        LOOP_HAVE_256(i, len,
                      accumulate_max(AVX256,
                                     curr, &val[i],
                                     max, &max[i]));

        LOOP_HAVE_128(i, len,
                      accumulate_max(AVX128,
                                     curr, &val[i],
                                     max, &max[i]));

        if(val[i] > max[i])
            max[i] = val[i];
        i++;
    }

    return 0;
}

int stat_oneshot_min(float *min, float *val, int len)
{
    int i;

    IF_HAVE_512(__m512 curr_512, max_512);
    IF_HAVE_256(__m256 curr_256, max_256);
    IF_HAVE_128(__m128 curr_, max_);

    if(!min || !val || len == 0)
    {
        err("Invalid args\n");
        return -EINVAL;
    }

    for(i = 0; i < len;)
    {
        LOOP_HAVE_512(i, len,
                      accumulate_min(AVX512,
                                     curr, &val[i],
                                     max, &min[i]));

        LOOP_HAVE_256(i, len,
                      accumulate_min(AVX256,
                                     curr, &val[i],
                                     max, &min[i]));

        LOOP_HAVE_128(i, len,
                      accumulate_min(AVX128,
                                     curr, &val[i],
                                     max, &min[i]));

        if(val[i] < min[i])
            min[i] = val[i];
        i++;
    }

    return 0;
}