cmake_minimum_required(VERSION 3.24)
project(MW2VRHookLoader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MW2VRHookLoader must be built with -A Win32 to match iw4sp.exe")
endif()

add_executable(MW2VRHookLoader
    MW2VRHookLoader.cpp
)

target_compile_definitions(MW2VRHookLoader PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(MW2VRHookLoader PRIVATE /W4 /permissive-)
endif()
