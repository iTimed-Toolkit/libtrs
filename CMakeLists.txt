cmake_minimum_required(VERSION 3.17)
project(libtrs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-O3 -Wall -Werror -mavx2")

include_directories(include)

add_library(libtrs lib/libtrs.c lib/cache.c
        lib/header.c lib/trace.c
        lib/list.c lib/render.c)
target_link_libraries(libtrs pthread)

add_library(libtfm transform/tfm.c transform/tfm_align.c
        transform/tfm_average.c transform/tfm_split_tvla.c
        transform/tfm_dpa.c transform/tfm_nop.c)
target_link_libraries(libtfm m)

add_executable(libtrs_test_cpu test.c)
target_link_libraries(libtrs_test_cpu libtrs libtfm)
target_compile_definitions(libtrs_test_cpu PRIVATE USE_GPU=0)
