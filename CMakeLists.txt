cmake_minimum_required(VERSION 3.17)
project(libtrs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall")

include_directories(include)

add_library(libtrs lib/libtrs.c lib/cache.c lib/header.c lib/trace.c)
add_library(libtfm transform/tfm.c transform/tfm_align.c
                    transform/tfm_average.c transform/tfm_split_tvla.c transform/tfm_dpa.c)

add_executable(libtrs_test_cpu dpa.c test.c)
target_link_libraries(libtrs_test_cpu m pthread libtrs libtfm)
target_compile_definitions(libtrs_test_cpu PRIVATE USE_GPU=0)