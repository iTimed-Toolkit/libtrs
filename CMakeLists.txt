cmake_minimum_required(VERSION 3.17)
project(libtrs C CUDA)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-O3 -Wall -mavx2")

include_directories(include)

add_library(trace lib/trace/trace_set.c lib/trace/cache.c
        lib/trace/header.c lib/trace/trace.c lib/trace/render.c)
target_link_libraries(trace pthread)

set(STATS_USE_GPU   0)
add_library(stats lib/stats/single.c lib/stats/dual.c
            lib/stats/single_array.c lib/stats/dual_array.c
            lib/stats/stats.c lib/stats/oneshot.c)

if(${STATS_USE_GPU} EQUAL 1)
    target_sources(stats PRIVATE lib/stats/gpu_single_array.cu
                    lib/stats/gpu_dual_array.cu)
endif()

target_compile_options(stats PRIVATE "-DUSE_GPU=${STATS_USE_GPU}")
target_link_libraries(stats m)

add_library(transform transform/tfm.c transform/tfm_align.c
        transform/tfm_average.c transform/trace/tfm_split_tvla.c
        transform/power_analysis/tfm_cpa.c transform/tfm_nop.c transform/system/tfm_save.c
        transform/power_analysis/tfm_io_correlation.c transform/trace/tfm_narrow.c
        transform/power_analysis/tfm_aes_intermediate.c transform/system/tfm_wait_on.c transform/system/tfm_visualize.c
        transform/block/tfm_block_maxabs.c transform/block/tfm_block_select.c transform/power_analysis/tfm_aes_knownkey.c)
target_link_libraries(transform pthread crypto m stats)

add_executable(libtrace_evaluate main.c)
target_link_libraries(libtrace_evaluate trace transform)