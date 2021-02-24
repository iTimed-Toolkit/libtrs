cmake_minimum_required(VERSION 3.17)
project(libtrs C CUDA)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CUDA_COMPILER /opt/cuda/bin/nvcc)
set(CMAKE_CUDA_STANDARD  14)

include_directories(include)

add_executable(libtrs_test_cpu dpa.c
        lib/libtrs.c test.c
        transform/tfm_align.c transform/tfm_average.c transform/tfm_split_tvla.c transform/tfm.c)
target_link_libraries(libtrs_test_cpu m pthread)
target_compile_definitions(libtrs_test_cpu PRIVATE USE_GPU=0)