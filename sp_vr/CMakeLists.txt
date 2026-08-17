cmake_minimum_required(VERSION 3.24)
project(MW2CampaignVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    openxr
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(openxr)

add_executable(MW2CampaignVR
    src/main.cpp
    src/GameProcess.cpp
    src/OpenXRHeadsetOpticalCenter.cpp
)

target_compile_definitions(MW2CampaignVR PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(MW2CampaignVR PRIVATE
    openxr_loader
    d3d11
    dxgi
    gdi32
    user32
    shell32
)

if(MSVC)
    # openxr_platform.h's D3D11 extension structs require the D3D11 COM types
    # to have been declared first. Force-include the SDK header for every TU so
    # this remains correct even when the OpenXR headers are included first.
    target_compile_options(MW2CampaignVR PRIVATE /W4 /permissive- /FId3d11.h)
endif()

add_custom_command(TARGET MW2CampaignVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:openxr_loader>
        $<TARGET_FILE_DIR:MW2CampaignVR>
)
