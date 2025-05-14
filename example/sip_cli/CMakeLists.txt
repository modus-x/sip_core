cmake_minimum_required(VERSION 3.25)

project(
  sip_console
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_COMPILER gcc-11 CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER g++-11 CACHE STRING "C++ compiler")


# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

add_subdirectory(extern/SDL3-3.2.12 EXCLUDE_FROM_ALL)

set(SIP_CORE_PATH "../sip_core" CACHE PATH "Path to a sip_core directory")
add_subdirectory(
  ${SIP_CORE_PATH}
  ${CMAKE_CURRENT_BINARY_DIR}/sip_core
)

add_executable(sip_console
    src/main.cpp
    src/SignalHandlers.cpp
)

target_include_directories(sip_console 
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(sip_console 
  PRIVATE 
    sip_core
    SDL3::SDL3
)