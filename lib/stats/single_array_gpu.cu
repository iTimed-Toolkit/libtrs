#include "__stat_internal.h"

__global__ void __do_accumulate_single_array(float count, float *val, int len, float *m, float *s)
{
    int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    if(id_x < len)
    {

    }
}

__global__ void __do_init_single_array(float *val, int len, float *m, float *s)
{
    int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    if(id_x < len)
    {
        m[id_x] = val[id_x];
        s[id_x] = 0;
    }
}


int __accumulate_single_array_gpu(struct accumulator *acc, float *val, int len)
{
    acc->count++;
    if(acc->count == 1)
    {

    }
}