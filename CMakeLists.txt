cmake_minimum_required(VERSION 3.17)
project(libtrs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-O3 -Wall -mavx2")

include_directories(include)

add_library(trace lib/libtrace.c lib/cache.c
        lib/header.c lib/trace.c
        lib/list.c lib/render.c)
target_link_libraries(trace pthread)

add_library(transform transform/tfm.c transform/tfm_align.c
        transform/tfm_average.c transform/tfm_split_tvla.c
        transform/power_analysis/tfm_cpa.c transform/tfm_nop.c transform/tfm_save.c
        lib/list.c transform/power_analysis/tfm_io_correlation.c transform/tfm_narrow.c transform/power_analysis/tfm_analyze_aes.c)
target_link_libraries(transform pthread crypto m)

add_executable(libtrs_test_cpu test.c aes.c)
target_link_libraries(libtrs_test_cpu trace transform)
target_compile_definitions(libtrs_test_cpu PRIVATE USE_GPU=0)
