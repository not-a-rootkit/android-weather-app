cmake_minimum_required(VERSION 3.22.1)
project(nativehelper)

add_library(nativehelper SHARED nativehelper.c)
find_library(log-lib log)
target_link_libraries(nativehelper ${log-lib})
