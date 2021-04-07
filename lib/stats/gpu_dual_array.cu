#include "__stat_internal.h"
#include "__trace_internal.h"

#include <errno.h>

#define THREADS_PER_BLOCK       32

struct dual_array_gpu_vars
{
    bool host_stale;
    cudaStream_t stream;
    float *val0, *val1, *m, *s, *cov;
};

__global__ void __do_accumulate_dual_array(float count,
                                           float *val0, float *val1,
                                           int len0, int len1,
                                           float *m, float *s, float *cov)
{
    int i;
    unsigned int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    float my_val, my_m, my_s, my_new_m, my_new_s;

    float per = len1 / len0;

    if(id_x < len0)
    {
        my_val = val0[id_x];
        my_m = m[id_x];
        my_s = s[id_x];

        my_new_m = my_m + (my_val - my_m) / count;
        my_new_s = my_s + ((my_val - my_m) * (my_val - my_new_m));

        m[id_x] = my_new_m;
        s[id_x] = my_new_s;

        for(i = 0; i < len1; i++)
        {
            my_val = val1[id_x];
            my_m = m[len0 + id_x];
            my_s = s[len0 + id_x];

            my_new_m = my_m + (my_val - my_m) / count;
            my_new_s = my_s + ((my_val - my_m) * my_val - my_new_m);

            m[len0 + id_x] = my_new_m;
        }
    }
}

__global__ void __do_init_dual_array(float *val0, float *val1,
                                     int len0, int len1,
                                     float *m, float *s, float *cov)
{
    unsigned int id_x = blockDim.x * blockIdx.x + threadIdx.x;
    if(id_x < len0)
    {
        m[id_x] = val0[id_x];
        s[id_x] = 0;
        cov[id_x] = 0;
    }
    else if(id_x < len0 + len1)
    {
        m[id_x] = val1[id_x - len0];
        s[id_x] = 0;
        cov[id_x] = 0;
    }
}

int __sync_dual_array_gpu(struct accumulator *acc)
{
    cudaError_t cuda_ret;
    struct dual_array_gpu_vars *vars =
            (struct dual_array_gpu_vars *) acc->gpu_vars;

    if(vars->host_stale)
    {
        cuda_ret = cudaMemcpyAsync(acc->m.a, vars->m,
                                   (acc->dim0 + acc->dim1) * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to copy m from GPU to host: %s\n", cudaGetErrorName(cuda_ret));
            return -EINVAL;
        }

        cuda_ret = cudaMemcpyAsync(acc->s.a, vars->s,
                                   (acc->dim0 + acc->dim1) * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to copy s from GPU to host: %s\n", cudaGetErrorName(cuda_ret));
            return -EINVAL;
        }

        cuda_ret = cudaMemcpyAsync(acc->cov.a, vars->cov,
                                   (acc->dim0 * acc->dim1) * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   vars->stream);
        if(cuda_ret != cudaSuccess)
        {
            err("Failed to copy cov from GPU to host: %s\n", cudaGetErrorName(cuda_ret));
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

int __accumulate_dual_array_gpu(struct accumulator *acc, float *val0, float *val1, int len0, int len1)
{
    cudaError_t cuda_ret;
    struct dual_array_gpu_vars *vars =
            (struct dual_array_gpu_vars *) acc->gpu_vars;

    dim3 block(THREADS_PER_BLOCK);

    acc->count++;
    cuda_ret = cudaMemcpyAsync(vars->val0, val0,
                               len0 * sizeof(float),
                               cudaMemcpyHostToDevice,
                               vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to memcpy val0 to GPU: %s\n", cudaGetErrorName(cuda_ret));
        return -EINVAL;
    }

    cuda_ret = cudaMemcpyAsync(vars->val1, val1,
                               len1 * sizeof(float),
                               cudaMemcpyHostToDevice,
                               vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to memcpy val1 to GPU: %s\n", cudaGetErrorName(cuda_ret));
        return -EINVAL;
    }

    if(acc->count == 1)
    {
        dim3 grid((len0 + len1) / THREADS_PER_BLOCK + 1);
        __do_init_dual_array<<<grid, block, 0, vars->stream>>>(vars->val0, vars->val1,
                                                               len0, len1,
                                                               vars->m, vars->s, vars->cov);
    }
    else
    {
        dim3 grid(len0 / THREADS_PER_BLOCK + 1);
        __do_accumulate_dual_array<<<grid, block, 0, vars->stream>>>(acc->count, vars->val0, vars->val1,
                                                                       len0, len1,
                                                                       vars->m, vars->s, vars->cov);
    }


    vars->host_stale = true;
    return 0;
}

int __init_dual_array_gpu(struct accumulator *acc, int num0, int num1)
{
    int ret;
    cudaError_t cuda_ret;
    struct dual_array_gpu_vars *vars;

    if(!acc)
    {
        err("Invalid accumulator\n");
        return -EINVAL;
    }

    vars = (struct dual_array_gpu_vars *)
            calloc(1, sizeof(struct dual_array_gpu_vars));
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

    cuda_ret = cudaMallocAsync(&vars->val0, num0 * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU val0 array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __destroy_stream;
    }
    cuda_ret = cudaMallocAsync(&vars->val1, num1 * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU val1 array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __destroy_stream;
    }

    cuda_ret = cudaMallocAsync(&vars->m, (num0 + num1) * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU m array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __free_bufs;
    }

    cuda_ret = cudaMallocAsync(&vars->s, (num0 + num1) * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU s array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __free_bufs;
    }

    cuda_ret = cudaMallocAsync(&vars->cov, (num0 * num1) * sizeof(float), vars->stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to allocate GPU cov array: %s\n", cudaGetErrorName(cuda_ret));
        ret = -ENOMEM;
        goto __free_bufs;
    }


    acc->gpu_vars = (void *) vars;
    return 0;

__free_bufs:
    if(vars->val0)
        cudaFreeAsync(vars->val0, vars->stream);

    if(vars->val1)
        cudaFreeAsync(vars->val1, vars->stream);

    if(vars->m)
        cudaFreeAsync(vars->m, vars->stream);

    if(vars->s)
        cudaFreeAsync(vars->s, vars->stream);

    if(vars->cov)
        cudaFreeAsync(vars->cov, vars->stream);

__destroy_stream:
    cudaStreamDestroy(vars->stream);

__free_vars:
    free(vars);
    return ret;
}