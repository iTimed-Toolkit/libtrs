#include "statistics.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main()
{
//    int i, j;
//    long  next;
//    float r[1024], mean, dev;
//
//    struct accumulator *acc_single_array;
//    stat_create_single_array(&acc_single_array, 1024);
//
//    srand(0);
//    for(i = 0; i < 1024; i++)
//    {
//        for(j = 0; j < 1024; j++)
//        {
//            next = random() % 256;
//            fprintf(stderr, "%li,", next);
//            r[j] = (float) next;
//        }
//
//        fprintf(stderr, "\n");
//        stat_accumulate_single_array(acc_single_array, r, 1024);
//    }
//    fprintf(stderr, "\n");
//
//    for(i = 0; i < 1024; i++)
//    {
//        stat_get_mean(acc_single_array, i, &mean);
//        fprintf(stderr, "%f,", mean);
//    }
//    fprintf(stderr, "\n");
//
//    for(i = 0; i < 1024; i++)
//    {
//        stat_get_dev(acc_single_array, i, &dev);
//        fprintf(stderr, "%f,", dev);
//    }
//    fprintf(stderr, "\n");

#define SIZE_1      6
#define SIZE_2      25

    struct accumulator *acc_dual;
    stat_create_dual_array(&acc_dual, SIZE_1, SIZE_2);

    srand(0);

    int i, j;
    long next;
    float r0[SIZE_1], r1[SIZE_2];

    float mean, dev, cov, pearson;
    float *pearson_all;

    for(i = 0; i < 15; i++)
    {
        for(j = 0; j < SIZE_1; j++)
        {
            next = random() % 256;
            fprintf(stderr, "%li,", next);
            r0[j] = (float) next;
        }
        fprintf(stderr, ",");
        for(j = 0; j < SIZE_2; j++)
        {
            next = random() % 256;
            fprintf(stderr, "%li,", next);
            r1[j] = (float) next;
        }

        fprintf(stderr, "\n");
        stat_accumulate_dual_array(acc_dual, r0, r1, SIZE_1, SIZE_2);
    }
    fprintf(stderr, "\n");

    for(j = 0; j < SIZE_1; j++)
    {
        stat_get_mean(acc_dual, j, &mean);
        fprintf(stderr, "%f,", mean);
    }
    fprintf(stderr, ",");
    for(j = 0; j < SIZE_2; j++)
    {
        stat_get_mean(acc_dual, SIZE_1 + j, &mean);
        fprintf(stderr, "%f,", mean);
    }
    fprintf(stderr, "\n");

    for(j = 0; j < SIZE_1; j++)
    {
        stat_get_dev(acc_dual, j, &dev);
        fprintf(stderr, "%f,", dev);
    }
    fprintf(stderr, ",");
    for(j = 0; j < SIZE_2; j++)
    {
        stat_get_dev(acc_dual, SIZE_1 + j, &dev);
        fprintf(stderr, "%f,", dev);
    }
    fprintf(stderr, "\n");

    stat_get_pearson_all(acc_dual, &pearson_all);

    for(i = 0; i < SIZE_1 * SIZE_2; i++)
    {
        if(i % SIZE_1 == 0)
            fprintf(stderr, "\n");

        stat_get_pearson(acc_dual, i, &pearson);
        fprintf(stderr, "%f,", pearson);

        if(fabsf(pearson - pearson_all[i]) > 0.0001)
        {
            fprintf(stderr, "\npearson mismatch index %i %f vs %f\n", i, pearson, pearson_all[i]);
        }
    }
    fprintf(stderr, "\n");
}