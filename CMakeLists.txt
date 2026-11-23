cmake_minimum_required(VERSION 3.30)

project(ShaderMakeAll LANGUAGES C CXX)

include(CMakeDependentOption)
include(FetchContent)

# Standalone defaults
if(PROJECT_IS_TOP_LEVEL)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set(SHADERMAKE_DEFAULT_BIN_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/bin")
    set(SHADERMAKE_DEFAULT_FIND_COMPILERS OFF)
else()
    set(SHADERMAKE_DEFAULT_BIN_OUTPUT_PATH "")
    set(SHADERMAKE_DEFAULT_FIND_COMPILERS ON)
endif()

set(SHADERMAKE_BIN_OUTPUT_PATH "${SHADERMAKE_DEFAULT_BIN_OUTPUT_PATH}" CACHE PATH "Output directory for the ShaderMake executable")

# Shader compilers run on the host, independently of the target architecture
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(SHADERMAKE_HOST_ARCH "aarch64")
    set(SHADERMAKE_WINDOWS_ARCH "arm64")
else()
    set(SHADERMAKE_HOST_ARCH "x86_64")
    set(SHADERMAKE_WINDOWS_ARCH "x64")
endif()

if(CMAKE_HOST_WIN32)
    set(SHADERMAKE_HOST_EXECUTABLE_SUFFIX ".exe")
else()
    set(SHADERMAKE_HOST_EXECUTABLE_SUFFIX "")
endif()

# Options
option(SHADERMAKE_FIND_COMPILERS "Find and download shader compilers" ${SHADERMAKE_DEFAULT_FIND_COMPILERS})
option(SHADERMAKE_FIND_DXC_VK "Find DXC in the Vulkan SDK or PATH" ON)
option(SHADERMAKE_FIND_SLANG "Download Slang from GitHub" OFF)
option(SHADERMAKE_TOOL "Build ShaderMake as a separate Release tool" OFF)

cmake_dependent_option(SHADERMAKE_FIND_FXC "Find FXC in the Windows SDK" ON "CMAKE_HOST_WIN32" OFF)
# Published DXC packages cover Windows and x86-64 Linux
cmake_dependent_option(SHADERMAKE_FIND_DXC "Download DXC from GitHub" ON "CMAKE_HOST_WIN32 OR (CMAKE_HOST_SYSTEM_NAME STREQUAL Linux AND SHADERMAKE_HOST_ARCH STREQUAL x86_64)" OFF)

set(SHADERMAKE_DXC_VERSION "v1.9.2609" CACHE STRING "DXC release tag")
set(SHADERMAKE_DXC_DATE "2026_09_29" CACHE STRING "DXC Windows archive date")
set(SHADERMAKE_DXC_LINUX_DATE "2026_09_28" CACHE STRING "DXC Linux archive date")
set(SHADERMAKE_SLANG_VERSION "2026.22.1" CACHE STRING "Slang release version")

# Declare binary packages without adding their CMake projects
set(SHADERMAKE_DEPENDENCIES)
if(SHADERMAKE_FIND_COMPILERS)
    if(SHADERMAKE_FIND_DXC)
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            set(SHADERMAKE_DXC_ARCHIVE "linux_dxc_${SHADERMAKE_DXC_LINUX_DATE}.x86_x64.tar.gz")
        else()
            set(SHADERMAKE_DXC_ARCHIVE "dxc_${SHADERMAKE_DXC_DATE}.zip")
        endif()

        FetchContent_Declare(shadermake_dxc
            URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${SHADERMAKE_DXC_VERSION}/${SHADERMAKE_DXC_ARCHIVE}"
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            DOWNLOAD_NO_PROGRESS TRUE
            SOURCE_SUBDIR "ignore CMakeLists.txt"
        )
        list(APPEND SHADERMAKE_DEPENDENCIES shadermake_dxc)
    endif()

    if(SHADERMAKE_FIND_SLANG)
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            set(SHADERMAKE_SLANG_PLATFORM "linux")
        elseif(CMAKE_HOST_APPLE)
            set(SHADERMAKE_SLANG_PLATFORM "macos")
        else()
            set(SHADERMAKE_SLANG_PLATFORM "windows")
        endif()

        FetchContent_Declare(shadermake_slang
            URL "https://github.com/shader-slang/slang/releases/download/v${SHADERMAKE_SLANG_VERSION}/slang-${SHADERMAKE_SLANG_VERSION}-${SHADERMAKE_SLANG_PLATFORM}-${SHADERMAKE_HOST_ARCH}.zip"
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            DOWNLOAD_NO_PROGRESS TRUE
            SOURCE_SUBDIR "ignore CMakeLists.txt"
        )
        list(APPEND SHADERMAKE_DEPENDENCIES shadermake_slang)
    endif()
endif()

if(SHADERMAKE_DEPENDENCIES)
    message(STATUS "ShaderMake: downloading ${SHADERMAKE_DEPENDENCIES}")
    FetchContent_MakeAvailable(${SHADERMAKE_DEPENDENCIES})
endif()

# Compiler paths remain available to parent projects
if(SHADERMAKE_FIND_COMPILERS)
    if(SHADERMAKE_FIND_DXC)
        if(CMAKE_HOST_WIN32)
            set(SHADERMAKE_DXC_PATH "${shadermake_dxc_SOURCE_DIR}/bin/${SHADERMAKE_WINDOWS_ARCH}/dxc.exe" CACHE INTERNAL "")
        else()
            set(SHADERMAKE_DXC_PATH "${shadermake_dxc_SOURCE_DIR}/bin/dxc" CACHE INTERNAL "")
        endif()
    endif()

    if(SHADERMAKE_FIND_SLANG)
        set(SHADERMAKE_SLANG_PATH "${shadermake_slang_SOURCE_DIR}/bin/slangc${SHADERMAKE_HOST_EXECUTABLE_SUFFIX}" CACHE INTERNAL "")
    endif()

    if(SHADERMAKE_FIND_DXC_VK)
        find_program(SHADERMAKE_DXC_VK_PATH
            NAMES dxc
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            NO_CMAKE_FIND_ROOT_PATH
            DOC "DXC from the Vulkan SDK or PATH"
        )
    endif()

    if(NOT SHADERMAKE_DXC_PATH AND SHADERMAKE_DXC_VK_PATH)
        set(SHADERMAKE_DXC_PATH "${SHADERMAKE_DXC_VK_PATH}" CACHE INTERNAL "")
    endif()
    if(NOT SHADERMAKE_DXC_VK_PATH AND SHADERMAKE_DXC_PATH)
        set(SHADERMAKE_DXC_VK_PATH "${SHADERMAKE_DXC_PATH}" CACHE INTERNAL "")
    endif()

    if(SHADERMAKE_FIND_FXC)
        cmake_host_system_information(RESULT SHADERMAKE_WINDOWS_SDK_ROOT
            QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
            VALUE "KitsRoot10"
        )

        set(SHADERMAKE_FXC_HINTS)
        if(SHADERMAKE_WINDOWS_SDK_ROOT)
            if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
                list(APPEND SHADERMAKE_FXC_HINTS "${SHADERMAKE_WINDOWS_SDK_ROOT}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/${SHADERMAKE_WINDOWS_ARCH}")
            endif()
            if(DEFINED ENV{WindowsSDKLibVersion})
                file(TO_CMAKE_PATH "$ENV{WindowsSDKLibVersion}" SHADERMAKE_WINDOWS_SDK_VERSION)
                list(APPEND SHADERMAKE_FXC_HINTS "${SHADERMAKE_WINDOWS_SDK_ROOT}/bin/${SHADERMAKE_WINDOWS_SDK_VERSION}/${SHADERMAKE_WINDOWS_ARCH}")
            endif()

            # Fall back to another installed SDK
            file(GLOB SHADERMAKE_WINDOWS_SDK_BINS LIST_DIRECTORIES TRUE "${SHADERMAKE_WINDOWS_SDK_ROOT}/bin/10.*")
            list(SORT SHADERMAKE_WINDOWS_SDK_BINS COMPARE NATURAL ORDER DESCENDING)
            foreach(SHADERMAKE_SDK_BIN IN LISTS SHADERMAKE_WINDOWS_SDK_BINS)
                list(APPEND SHADERMAKE_FXC_HINTS "${SHADERMAKE_SDK_BIN}/${SHADERMAKE_WINDOWS_ARCH}")
            endforeach()
        endif()

        find_program(SHADERMAKE_FXC_PATH
            NAMES fxc
            HINTS ${SHADERMAKE_FXC_HINTS}
            NO_CMAKE_FIND_ROOT_PATH
            REQUIRED
            DOC "FXC from the Windows SDK"
        )
    endif()
endif()

# Shader blob library
if(NOT TARGET ShaderMakeBlob)
    add_library(ShaderMakeBlob STATIC)
    target_sources(ShaderMakeBlob
        PRIVATE
            "ShaderMake/ShaderBlob.h"
            "ShaderMake/ShaderBlob.cpp"
    )
    target_include_directories(ShaderMakeBlob PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(ShaderMakeBlob PUBLIC cxx_std_17)
    target_compile_options(ShaderMakeBlob
        PRIVATE
            "$<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wextra>"
            "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:/W4;/WX>"
    )
    target_compile_definitions(ShaderMakeBlob
        PRIVATE
            "$<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN;NOMINMAX;_CRT_SECURE_NO_WARNINGS>"
    )
    set_target_properties(ShaderMakeBlob
        PROPERTIES
            CXX_EXTENSIONS OFF
            POSITION_INDEPENDENT_CODE ON
            FOLDER "ShaderMake"
    )
endif()

# A separate host build keeps the tool in Release across parent configurations
if(SHADERMAKE_TOOL)
    include(ExternalProject)

    get_property(SHADERMAKE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(SHADERMAKE_MULTI_CONFIG)
        set(SHADERMAKE_TOOL_PATH "${CMAKE_CURRENT_BINARY_DIR}/build/Release/ShaderMake${SHADERMAKE_HOST_EXECUTABLE_SUFFIX}")
    else()
        set(SHADERMAKE_TOOL_PATH "${CMAKE_CURRENT_BINARY_DIR}/build/ShaderMake${SHADERMAKE_HOST_EXECUTABLE_SUFFIX}")
    endif()
    set(SHADERMAKE_PATH "${SHADERMAKE_TOOL_PATH}" CACHE INTERNAL "")

    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        if(SHADERMAKE_HOST_ARCH STREQUAL "aarch64")
            set(SHADERMAKE_GENERATOR_PLATFORM "ARM64")
        else()
            set(SHADERMAKE_GENERATOR_PLATFORM "x64")
        endif()
    else()
        set(SHADERMAKE_GENERATOR_PLATFORM "")
    endif()

    set(SHADERMAKE_TOOL_CMAKE_ARGS)
    if(NOT SHADERMAKE_MULTI_CONFIG)
        list(APPEND SHADERMAKE_TOOL_CMAKE_ARGS "-DCMAKE_BUILD_TYPE:STRING=Release")
    endif()
    if(CMAKE_MAKE_PROGRAM)
        list(APPEND SHADERMAKE_TOOL_CMAKE_ARGS "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}")
    endif()

    ExternalProject_Add(ShaderMake
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ShaderMake"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/build"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${SHADERMAKE_GENERATOR_PLATFORM}"
        CMAKE_GENERATOR_TOOLSET "${CMAKE_GENERATOR_TOOLSET}"
        CMAKE_GENERATOR_INSTANCE "${CMAKE_GENERATOR_INSTANCE}"
        CMAKE_ARGS ${SHADERMAKE_TOOL_CMAKE_ARGS}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target ShaderMake --config Release
        BUILD_BYPRODUCTS "${SHADERMAKE_PATH}"
        BUILD_ALWAYS ON
        INSTALL_COMMAND ""
        STEP_TARGETS build
    )
    set_target_properties(ShaderMake ShaderMake-build PROPERTIES FOLDER "ShaderMake")
else()
    add_subdirectory(ShaderMake)
    set(SHADERMAKE_PATH "ShaderMake" CACHE INTERNAL "")
endif()

foreach(SHADERMAKE_OUTPUT IN ITEMS SHADERMAKE_PATH SHADERMAKE_FXC_PATH SHADERMAKE_DXC_PATH SHADERMAKE_DXC_VK_PATH SHADERMAKE_SLANG_PATH)
    message(STATUS "${SHADERMAKE_OUTPUT} = '${${SHADERMAKE_OUTPUT}}'")
endforeach()
