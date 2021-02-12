cmake_minimum_required(VERSION 3.17)
project(libtrs C CUDA)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CUDA_COMPILER /opt/cuda/bin/nvcc)
set(CMAKE_CUDA_STANDARD  14)

add_executable(libtrs_test_cpu dpa.c libtrs.c test.c)
target_link_libraries(libtrs_test_cpu m pthread)

#add_executable(libtrs_test_gpu dpa.c libtrs.c dpa_accel.cu test.c)
#target_link_libraries(libtrs_test_gpu m pthread)