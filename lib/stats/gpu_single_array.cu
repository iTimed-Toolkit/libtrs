#include "__stat_internal.h"
#include "__trace_internal.h"

#include <errno.h>
#define THREADS_PER_BLOCK       32

struct single_array_gpu_vars
{
    bool host_stale;
    cudaStream_t stream;
    float *val, *m, *s;
};

__global__ void __do_accumulate_single_array(float count, float *val, int len, float *m, float *s)
{
    unsigned int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    float my_val, my_m, my_s, my_new_m, my_new_s;

    if(id_x < len)
    {
        my_val = val[id_x];
        my_m = m[id_x];
        my_s = s[id_x];

        my_new_m = my_m + (my_val - my_m) / count;
        my_new_s = my_s + ((my_val - my_m) * (my_val - my_new_m));

        m[id_x] = my_new_m;
        s[id_x] = my_new_s;
    }
}

__global__ void __do_init_single_array(float *val, int len, float *m, float *s)
{
    unsigned int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    if(id_x < len)
    {
        m[id_x] = val[id_x];
        s[id_x] = 0;
    }
}

int __sync_single_array_gpu(struct accumulator *acc)
{
    cudaError_t cuda_ret;
    struct single_array_gpu_vars *vars =
            (struct single_array_gpu_vars *) acc->gpu_vars;

    if(vars->host_stale)
    {
        cuda_ret = cudaMemcpyAsync(acc->m.a, vars->m,
                                   acc->dim0 * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to copy m from GPU to host: %s\n", cudaGetErrorName(cuda_ret));
            return -EINVAL;
        }

        cuda_ret = cudaMemcpyAsync(acc->s.a, vars->s,
                                   acc->dim0 * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to copy s from GPU to host: %s\n", cudaGetErrorName(cuda_ret));
            return -EINVAL;
        }

        cuda_ret = cudaStreamSynchronize(vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to synchronize stream: %s\n", cudaGetErrorName(cuda_ret));
            return -EINVAL;
        }

        vars->host_stale = false;
    }

    return 0;
}

int __accumulate_single_array_gpu(struct accumulator *acc, float *val, int len)
{
    cudaError_t cuda_ret;
    struct single_array_gpu_vars *vars =
            (struct single_array_gpu_vars *) acc->gpu_vars;

    dim3 grid(len / THREADS_PER_BLOCK + 1);
    dim3 block(THREADS_PER_BLOCK);

    acc->count++;
    cuda_ret = cudaMemcpyAsync(vars->val, val,
                               len * sizeof(float),
                               cudaMemcpyHostToDevice,
                               vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to memcpy values to GPU: %s\n", cudaGetErrorName(cuda_ret));
        return -EINVAL;
    }

    if(acc->count == 1)
        __do_init_single_array<<<grid, block, 0, vars->stream>>>(vars->val, len, vars->m, vars->s);
    else
        __do_accumulate_single_array<<<grid, block, 0, vars->stream>>>(acc->count, vars->val,
                                                                       len, vars->m, vars->s);

    vars->host_stale = true;
    return 0;
}

int __init_single_array_gpu(struct accumulator *acc, int num)
{
    int ret;
    cudaError_t cuda_ret;
    struct single_array_gpu_vars *vars;

    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    vars = (struct single_array_gpu_vars *)
            calloc(1, sizeof(struct single_array_gpu_vars));
    if(!vars)
    {
        err("Failed to allocate memory for GPU vars\n");
        return -ENOMEM;
    }

    vars->host_stale = false;

    cuda_ret = cudaStreamCreate(&vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to create cuda stream: %s\n", cudaGetErrorName(cuda_ret));
        ret = -EINVAL;
        goto __free_vars;
    }

    cuda_ret = cudaMallocAsync(&vars->val, num * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU val array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __destroy_stream;
    }

    cuda_ret = cudaMallocAsync(&vars->m, num * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU m array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __free_bufs;
    }

    cuda_ret = cudaMallocAsync(&vars->s, num * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU s array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __free_bufs;
    }

    acc->gpu_vars = (void *) vars;
    return 0;

__free_bufs:
    if(vars->val)
        cudaFreeAsync(vars->val, vars->stream);

    if(vars->m)
        cudaFreeAsync(vars->m, vars->stream);

    if(vars->s)
        cudaFreeAsync(vars->s, vars->stream);

__destroy_stream:
    cudaStreamDestroy(vars->stream);

__free_vars:
    free(vars);
    return ret;
}
