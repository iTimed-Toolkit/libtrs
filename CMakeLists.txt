cmake_minimum_required(VERSION 3.14)
project(libtrs C CUDA)
set(CMAKE_C_STANDARD 11)

if(UNIX)
    message("Configuring for Linux")
    set(CMAKE_C_FLAGS "-Wall -mavx2 -g")

    set(LT_ZLIB "z")
    set(LT_MATH "m")
    set(LT_SSL "crypto")
    set(LT_THREADS "pthread")
    set(LT_NET "")
elseif(WIN32)
    message("Configuring for Windows")
    set(CMAKE_C_FLAGS "/arch:AVX2")

    add_subdirectory(contrib/zlib)

    include_directories(contrib/zlib)
    include_directories(${CMAKE_CURRENT_BINARY_DIR}/contrib/zlib)
    link_directories(${CMAKE_CURRENT_BINARY_DIR}/contrib/zlib)

    include_directories("C:\\Program Files\\OpenSSL-Win64\\include")
    link_directories("C:\\Program Files\\OpenSSL-Win64\\lib")

    set(LT_ZLIB "zlibstatic")
    set(LT_MATH "")
    set(LT_SSL "libcrypto")
    set(LT_THREADS "")
    set(LT_NET "ws2_32")
endif()

include_directories(include)

# core trace library
add_library(trace STATIC lib/trace/trace_set.c lib/trace/cache.c  lib/trace/trace.c
        lib/trace/frontend/render.c lib/trace/frontend/export.c
        lib/trace/backend/backend.c lib/trace/backend/riscure_trs.c
        lib/trace/backend/backend_trs.c lib/trace/backend/backend_ztrs.c lib/trace/backend/backend_net.c
        lib/platform/secure_socket.c lib/platform/platform_socket.c
        lib/platform/platform_sem.c lib/platform/platform_thread.c)
target_link_libraries(trace ${LT_THREADS} ${LT_NET} ${LT_ZLIB} ${LT_SSL})

# statistics
set(STATS_USE_GPU 1)
add_library(stats STATIC lib/stats/single.c lib/stats/dual.c lib/stats/single_array.c lib/stats/dual_array.c
        lib/stats/pattern_match.c lib/stats/stats.c)

if (${STATS_USE_GPU} EQUAL 1)
    target_sources(stats PRIVATE lib/stats/gpu_pattern_match.cu
            lib/stats/gpu_dual_array.cu)

    cmake_policy(SET CMP0104 OLD)
    set_property(TARGET stats PROPERTY CUDA_SEPARABLE_COMPILATION ON)
    set_property(TARGET stats PROPERTY CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    set_property(TARGET stats PROPERTY CUDA_ARCHITECTURES OFF)
endif ()

target_compile_options(stats PRIVATE "-DUSE_GPU=${STATS_USE_GPU}")
target_link_libraries(stats ${LT_MATH})

# crypto
add_library(crypto_utils STATIC lib/crypto/aes.c)
target_link_libraries(crypto_utils ${LT_SSL})

# transformations
add_library(transform STATIC transform/tfm.c transform/tfm_align.c transform/tfm_match.c
        transform/tfm_average.c transform/trace/tfm_split_tvla.c
        transform/power_analysis/tfm_cpa.c transform/tfm_nop.c transform/system/tfm_save.c
        transform/power_analysis/tfm_io_correlation.c transform/trace/tfm_narrow.c
        transform/power_analysis/tfm_aes_intermediate.c transform/system/tfm_wait_on.c transform/system/tfm_visualize.c
        transform/power_analysis/tfm_aes_knownkey.c
        transform/system/tfm_synchronize.c transform/trace/tfm_append.c transform/tfm_verify.c
        transform/tfm_block.c transform/block/tfm_reduce_along.c
        transform/block/tfm_select_along.c transform/block/tfm_sort_along.c
        transform/extract/tfm_extract_pattern.c transform/extract/tfm_extract_timing.c transform/extract/extract_internal.c)
target_link_libraries(transform ${LT_THREADS} ${LT_MATH} crypto_utils stats)

# frontend
add_executable(libtrace_evaluate main.c)
target_link_libraries(libtrace_evaluate trace transform stats)

add_executable(libtrace_misc misc.c)
target_link_libraries(libtrace_misc trace transform stats)