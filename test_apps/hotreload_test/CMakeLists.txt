# Basic Hot Reload Example
# This example demonstrates the hot reload functionality

cmake_minimum_required(VERSION 3.16)

# Add test_host directory to component search path
set(EXTRA_COMPONENT_DIRS test_host)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main espcoredump)
project(hotreload_basic_example)

# Treat warnings as errors for this project's components
idf_build_set_property(COMPILE_OPTIONS "-Werror" APPEND)
