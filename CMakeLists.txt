cmake_minimum_required(VERSION 3.14)
project(libtrs C CUDA)

cmake_policy(SET CMP0104 OLD)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -mavx2 -O3")

include_directories(include)

# core trace library
add_library(trace STATIC lib/trace/trace_set.c lib/trace/cache.c  lib/trace/trace.c
        lib/trace/network/secure_socket.c
        lib/trace/frontend/render.c lib/trace/frontend/export.c
        lib/trace/backend/backend.c lib/trace/backend/riscure_trs.c
        lib/trace/backend/backend_trs.c lib/trace/backend/backend_ztrs.c lib/trace/backend/backend_net.c)
#target_compile_options(trace PRIVATE "-O3")
target_link_libraries(trace pthread crypto z)

# statistics
set(STATS_USE_GPU 0)
add_library(stats STATIC lib/stats/single.c lib/stats/dual.c
        lib/stats/single_array.c lib/stats/dual_array.c
        lib/stats/stats.c lib/stats/oneshot.c lib/stats/gpu_pattern_match.cu)
set_property(TARGET stats PROPERTY CUDA_SEPARABLE_COMPILATION ON)
set_property(TARGET stats PROPERTY CUDA_RESOLVE_DEVICE_SYMBOLS ON)
set_property(TARGET stats PROPERTY CUDA_ARCHITECTURES OFF)

#if (${STATS_USE_GPU} EQUAL 1)
#    target_sources(stats PRIVATE )
#endif ()

target_compile_options(stats PRIVATE "-DUSE_GPU=${STATS_USE_GPU}")
target_link_libraries(stats m)

# crypto
add_library(crypto_utils STATIC lib/crypto/aes.c)
#target_compile_options(crypto_utils PRIVATE "-O3")
target_link_libraries(crypto_utils crypto)

# transformations
add_library(transform STATIC transform/tfm.c transform/tfm_align.c transform/tfm_match.c
        transform/tfm_average.c transform/trace/tfm_split_tvla.c
        transform/power_analysis/tfm_cpa.c transform/tfm_nop.c transform/system/tfm_save.c
        transform/power_analysis/tfm_io_correlation.c transform/trace/tfm_narrow.c
        transform/power_analysis/tfm_aes_intermediate.c transform/system/tfm_wait_on.c transform/system/tfm_visualize.c
        transform/power_analysis/tfm_aes_knownkey.c
        transform/system/tfm_synchronize.c transform/trace/tfm_append.c transform/tfm_verify.c
        transform/tfm_block.c transform/block/tfm_reduce_along.c transform/block/tfm_select_along.c
        transform/extract/tfm_extract_pattern.c)
#target_compile_options(transform PRIVATE "-O3")
target_link_libraries(transform pthread crypto_utils m stats)

# frontend
add_executable(libtrace_evaluate main.c)
#target_compile_options(libtrace_evaluate PRIVATE "-O3")
target_link_libraries(libtrace_evaluate trace transform stats)