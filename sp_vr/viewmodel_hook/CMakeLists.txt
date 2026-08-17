cmake_minimum_required(VERSION 3.24)
project(MW2VRViewmodelHook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MW2VRViewmodelHook must be built with -A Win32 because iw4sp.exe is 32-bit")
endif()

add_library(MW2VRViewmodelHook SHARED
    MW2VRFinalViewmodelPlacement.cpp
    MW2VRLiveHook.cpp
    MW2VRHandsFilter.cpp
    MW2VRViewmodelHook.def
)

target_compile_definitions(MW2VRViewmodelHook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(MW2VRViewmodelHook PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

if(MSVC)
    target_compile_options(MW2VRViewmodelHook PRIVATE /W4 /permissive-)
endif()
