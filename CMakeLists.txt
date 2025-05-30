cmake_minimum_required(VERSION 3.15)

include(FetchContent)
include(ExternalProject)
include(CMakeDependentOption)

project(ShaderMakeAll LANGUAGES C CXX)

# Is submodule?
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
    set(IS_SUBMODULE OFF)
else()
    set(IS_SUBMODULE ON)
endif()

# Cached
if(IS_SUBMODULE)
    set(SHADERMAKE_BIN_OUTPUT_PATH "" CACHE STRING "")
else()
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "")
    set(SHADERMAKE_BIN_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/bin" CACHE STRING "")
endif()

cmake_dependent_option(SHADERMAKE_FIND_FXC "Find FXC in Windows SDK and populate 'SHADERMAKE_FXC_PATH' variable" ON "WIN32" OFF)
cmake_dependent_option(SHADERMAKE_FIND_DXC "Download DXC from GitHub and populate 'SHADERMAKE_DXC_PATH' variable" ON "NOT APPLE" OFF)

option(SHADERMAKE_FIND_DXC_VK "Find DXC in Vulkan SDK and populate 'SHADERMAKE_DXC_VK_PATH' variable" ON)
option(SHADERMAKE_FIND_SLANG "Download Slang from GitHub and populate 'SHADERMAKE_SLANG_PATH' variable" OFF)
option(SHADERMAKE_FIND_COMPILERS "Master switch" ${IS_SUBMODULE})
option(SHADERMAKE_TOOL "Use ShaderMake as an external tool and hide the executable from the parent project IDE" OFF)

set(SHADERMAKE_DXC_VERSION "v1.8.2502" CACHE STRING "DXC to download from 'GitHub/DirectXShaderCompiler' releases")
set(SHADERMAKE_DXC_DATE "2025_02_20" CACHE STRING "DXC release date") # DXC releases on GitHub have this in download links :(
set(SHADERMAKE_SLANG_VERSION "2025.9.1" CACHE STRING "Slang to download from 'GitHub/Shader-slang' releases")

# Globals?
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile options
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(COMPILE_OPTIONS -Wextra)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(COMPILE_OPTIONS -Wextra)
elseif(MSVC)
    set(COMPILE_OPTIONS /W4 /WX)
else()
    message(WARNING "ShaderMake: Unknown compiler!")
endif()

if((CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64") OR(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64"))
    set(WINDOWS_ARCH "arm64")
else()
    set(WINDOWS_ARCH "x64")
endif()

# Find compilers
if(SHADERMAKE_FIND_COMPILERS)
    # DXC (GitHub)
    if(SHADERMAKE_FIND_DXC)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(DXC_SUBSTRING "linux_dxc_${SHADERMAKE_DXC_DATE}.x86_64.tar.gz")
        else()
            set(DXC_SUBSTRING "dxc_${SHADERMAKE_DXC_DATE}.zip")
        endif()

        set(DXC_DOWNLOAD_LINK https://github.com/microsoft/DirectXShaderCompiler/releases/download/${SHADERMAKE_DXC_VERSION}/${DXC_SUBSTRING})

        FetchContent_Declare(
            dxc
            DOWNLOAD_EXTRACT_TIMESTAMP 1
            DOWNLOAD_NO_PROGRESS 1
            URL ${DXC_DOWNLOAD_LINK}
        )

        message(STATUS "ShaderMake: downloading DXC ${SHADERMAKE_DXC_VERSION}...")

        FetchContent_MakeAvailable(dxc)

        if(WIN32)
            set(SHADERMAKE_DXC_PATH "${dxc_SOURCE_DIR}/bin/${WINDOWS_ARCH}/dxc.exe" CACHE INTERNAL "")
        else()
            set(SHADERMAKE_DXC_PATH "${dxc_SOURCE_DIR}/bin/dxc" CACHE INTERNAL "")
        endif()
    endif()

    # Slang (GitHub)
    if(SHADERMAKE_FIND_SLANG)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(SLANG_SUBSTRING "-linux")
        elseif(APPLE)
            set(SLANG_SUBSTRING "-macos")
        else()
            set(SLANG_SUBSTRING "-windows")
        endif()

        if((CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64") OR(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64"))
            set(SLANG_SUBSTRING "${SLANG_SUBSTRING}-aarch64")
        else()
            set(SLANG_SUBSTRING "${SLANG_SUBSTRING}-x86_64")
        endif()

        set(SLANG_DOWNLOAD_LINK https://github.com/shader-slang/slang/releases/download/v${SHADERMAKE_SLANG_VERSION}/slang-${SHADERMAKE_SLANG_VERSION}${SLANG_SUBSTRING}.zip)

        FetchContent_Declare(
            slang
            DOWNLOAD_EXTRACT_TIMESTAMP 1
            DOWNLOAD_NO_PROGRESS 1
            URL ${SLANG_DOWNLOAD_LINK}
        )

        message(STATUS "ShaderMake: downloading Slang ${SHADERMAKE_SLANG_VERSION}...")

        FetchContent_MakeAvailable(slang)

        if(WIN32)
            set(SHADERMAKE_SLANG_PATH "${slang_SOURCE_DIR}/bin/slangc.exe" CACHE INTERNAL "")
        else()
            set(SHADERMAKE_SLANG_PATH "${slang_SOURCE_DIR}/bin/slangc" CACHE INTERNAL "")
        endif()
    endif()

    # DXC (Vulkan SDK)
    if(SHADERMAKE_FIND_DXC_VK)
        if(WIN32)
            find_program(SHADERMAKE_DXC_VK_PATH "$ENV{VULKAN_SDK}/Bin/dxc")
        else()
            find_program(SHADERMAKE_DXC_VK_PATH "dxc")
        endif()

        # Soft-fallback to a valid path
        if(NOT SHADERMAKE_DXC_PATH AND SHADERMAKE_DXC_VK_PATH)
            set(SHADERMAKE_DXC_PATH ${SHADERMAKE_DXC_VK_PATH} CACHE INTERNAL "")
        endif()
    endif()

    # Soft-fallback to a valid path
    if(NOT SHADERMAKE_DXC_VK_PATH AND SHADERMAKE_DXC_PATH)
        set(SHADERMAKE_DXC_VK_PATH ${SHADERMAKE_DXC_PATH} CACHE INTERNAL "")
    endif()

    # FXC (Windows SDK)
    if(SHADERMAKE_FIND_FXC)
        if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            set(WINDOWS_SDK_VERSION ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})
        elseif(DEFINED ENV{WindowsSDKLibVersion})
            string(REGEX REPLACE "\\\\$" "" WINDOWS_SDK_VERSION "$ENV{WindowsSDKLibVersion}")
        else()
            message(FATAL_ERROR "ShaderMake: WindowsSDK is not installed (CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION is not defined; WindowsSDKLibVersion is '$ENV{WindowsSDKLibVersion}')!")
        endif()

        get_filename_component(WINDOWS_SDK_ROOT "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
        set(WINDOWS_SDK_BIN "${WINDOWS_SDK_ROOT}/bin/${WINDOWS_SDK_VERSION}/${WINDOWS_ARCH}")

        find_program(SHADERMAKE_FXC_PATH "${WINDOWS_SDK_BIN}/fxc")
    endif()
endif()

# ShaderMakeBlob
add_library(ShaderMakeBlob STATIC
    "ShaderMake/ShaderBlob.h"
    "ShaderMake/ShaderBlob.cpp"
)
target_include_directories(ShaderMakeBlob PUBLIC "")
target_compile_options(ShaderMakeBlob PRIVATE ${COMPILE_OPTIONS})
set_target_properties(ShaderMakeBlob PROPERTIES FOLDER "ShaderMake" POSITION_INDEPENDENT_CODE ON)

# ShaderMake
if(SHADERMAKE_TOOL)
    if(NOT CMAKE_CONFIGURATION_TYPES)
        set(ARGS "-DCMAKE_BUILD_TYPE=Release")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(SHADERMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/build/Release/ShaderMake${CMAKE_EXECUTABLE_SUFFIX}" CACHE INTERNAL "")
    else()
        set(SHADERMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/build/ShaderMake${CMAKE_EXECUTABLE_SUFFIX}" CACHE INTERNAL "")
    endif()

    ExternalProject_Add(
        ShaderMake
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ShaderMake"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/build"
        CMAKE_ARGS "${ARGS}"
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ShaderMake --config Release
        BUILD_BYPRODUCTS ${SHADERMAKE_PATH}
        INSTALL_COMMAND ""
        STEP_TARGETS build
        COMMENT "Building ShaderMake (tool)..."
    )
else()
    add_subdirectory(ShaderMake)

    set(SHADERMAKE_PATH "ShaderMake" CACHE INTERNAL "")
endif()

# Done
message(STATUS "SHADERMAKE_PATH = '${SHADERMAKE_PATH}'")
message(STATUS "SHADERMAKE_FXC_PATH = '${SHADERMAKE_FXC_PATH}'")
message(STATUS "SHADERMAKE_DXC_PATH = '${SHADERMAKE_DXC_PATH}'")
message(STATUS "SHADERMAKE_DXC_VK_PATH = '${SHADERMAKE_DXC_VK_PATH}'")
message(STATUS "SHADERMAKE_SLANG_PATH = '${SHADERMAKE_SLANG_PATH}'")
