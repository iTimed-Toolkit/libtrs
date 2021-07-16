#include "statistics.h"

#include "__trace_internal.h"

#include <stdlib.h>

#define BLOCKDIM    128

__global__ void __do_gpu_pattern_match(const float *data, int data_len,
                                       const float *pattern, int pattern_len,
                                       float s_pattern, float *res)
{
    int i;
    size_t block_x, my_x;

    // for various data values
    float data_val, m_data,
            m_new_data, s_data;

    // for accumulated covariance
    float cov, pearson, len, p;

    block_x = blockDim.x * blockIdx.x;
    my_x = block_x + threadIdx.x;

    // length of data might not be evenly divisible by BLOCKDIM
    if(my_x < (data_len - pattern_len))
    {
        // unrolled first iteration (different)
        m_data = data[my_x];
        s_data = 0.0f;
        cov = 0.0f;

        i = 1;
        while(i < pattern_len)
        {
            p = pattern[i];
            data_val = data[my_x + i];

            // update covariance and related stats
            m_new_data = m_data + (data_val - m_data) / (float) (i + 1);
            s_data += ((data_val - m_data) * (data_val - m_new_data));
            cov += ((data_val - m_data) * p);
            m_data = m_new_data;
            i++;
        }

        len = (float) (pattern_len - 1);
        pearson = cov / (len * sqrtf(s_data / len) * sqrtf(s_pattern / len));
        res[my_x] = pearson;
    }
}

int gpu_pattern_preprocess(float *pattern, int pattern_len, float **out, float *var)
{
    int i;
    cudaError_t ret;
    float val, s,
            m, m_new, *res, *res_gpu;

    res = (float *) calloc(pattern_len, sizeof(float));
    if(!res)
    {
        err("Failed to allocate result array\n");
        return -ENOMEM;
    }

    ret = cudaMalloc(&res_gpu, pattern_len * sizeof(float));
    if(ret != cudaSuccess)
    {
        err("Failed to allocate result array on GPU: %s\n", cudaGetErrorName(ret));
        free(res);
        return -EINVAL;
    }

    m = pattern[0];
    s = 0;
    res[0] = 0.0f;

    for(i = 1; i < pattern_len; i++)
    {
        val = pattern[i];

        m_new = m + (val - m) / (float) (i + 1);
        s += ((val - m) * (val - m_new));
        m = m_new;

        res[i] = (val - m_new);
    }

    ret = cudaMemcpy(res_gpu, res, pattern_len * sizeof(float), cudaMemcpyHostToDevice);
    if(ret != cudaSuccess)
    {
        err("Failed to move preprocessed pattern to GPU\n");
        cudaFree(res_gpu);
        free(res);
        return -EINVAL;
    }

    free(res);
    *out = res_gpu;
    *var = s;
    return 0;

}

int gpu_pattern_free(float *pattern)
{
    cudaFree(pattern);
    return 0;
}

int gpu_pattern_match(float *data, int data_len, float *pattern, int pattern_len, float s_pattern, float **pearson)
{
    int ret;
    float *data_gpu = NULL, *res_gpu = NULL, *res = NULL;

    cudaError_t cuda_ret;
    dim3 grid, block;

    cudaStream_t stream;
    float time;
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    res = (float *) calloc(data_len - pattern_len, sizeof(float));
    if(!res)
    {
        err("Failed to allocate result array\n");
        return -ENOMEM;
    }

    cuda_ret = cudaStreamCreate(&stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to create cuda stream: %s\n", cudaGetErrorName(cuda_ret));
        free(res);
        ret = -EINVAL;
        goto __done;
    }

    cuda_ret = cudaMallocAsync(&data_gpu, data_len * sizeof(float), stream);
    if(cuda_ret == cudaSuccess)
        cuda_ret = cudaMallocAsync(&res_gpu, (data_len - pattern_len) * sizeof(float), stream);
    if(cuda_ret == cudaSuccess)
        cuda_ret = cudaMemcpyAsync(data_gpu, data, data_len * sizeof(float), cudaMemcpyHostToDevice, stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to initialize some GPU data: %s\n", cudaGetErrorName(cuda_ret));
        ret = -EINVAL;
        goto __free_gpu;
    }

    block.x = BLOCKDIM;
    grid.x = (data_len / BLOCKDIM) + 1;

    cudaEventRecord(start, stream);

    __do_gpu_pattern_match<<<grid, block, 0, stream>>>(data_gpu, data_len, pattern, pattern_len, s_pattern, res_gpu);
    cudaEventRecord(stop, stream);
    cudaEventSynchronize(stop);
    cudaEventElapsedTime(&time, start, stop);

    debug("gpu elapsed %f ms\n", time);
    cuda_ret = cudaMemcpyAsync(res, res_gpu, (data_len - pattern_len) * sizeof(float), cudaMemcpyDeviceToHost, stream);
    if(cuda_ret != cudaSuccess)
    {
        err("Failed to retrieve results from GPU: %s\n", cudaGetErrorName(cuda_ret));
        ret = -EINVAL;
        goto __free_gpu;
    }

    *pearson = res;
    ret = 0;
__free_gpu:
    if(data_gpu)
        cudaFreeAsync(data_gpu, stream);

    if(res_gpu)
        cudaFreeAsync(res_gpu, stream);

    cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);
__done:
    return ret;
}
