cmake_minimum_required(VERSION 3.17)
project(libtrs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-O0 -Wall -mavx2")

include_directories(include)

add_library(trace lib/trace/trace_set.c lib/trace/cache.c
        lib/trace/header.c lib/trace/trace.c
        lib/trace/list.c lib/trace/render.c)
target_link_libraries(trace pthread)

add_library(stats lib/stats/single.c lib/stats/dual.c
        lib/stats/single_array.c lib/stats/dual_array.c
        lib/stats/stats.c)
target_link_libraries(stats m)

add_library(transform transform/tfm.c transform/tfm_align.c
        transform/tfm_average.c transform/tfm_split_tvla.c
        transform/power_analysis/tfm_cpa.c transform/tfm_nop.c transform/tfm_save.c
        lib/trace/list.c transform/power_analysis/tfm_io_correlation.c transform/tfm_narrow.c transform/power_analysis/tfm_analyze_aes.c)
target_link_libraries(transform pthread crypto m stats)

add_executable(libtrs_test_cpu test.c aes.c)
target_link_libraries(libtrs_test_cpu trace transform)

add_executable(test_accumulate test_accumulators.c)
target_link_libraries(test_accumulate stats)
