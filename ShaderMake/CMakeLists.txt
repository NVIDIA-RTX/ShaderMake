cmake_minimum_required(VERSION 3.30)

project(ShaderMake LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

set(SHADERMAKE_TARGETS ShaderMake)
if(NOT TARGET ShaderMakeBlob)
    get_filename_component(SHADERMAKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_library(ShaderMakeBlob STATIC)
    target_sources(ShaderMakeBlob
        PRIVATE
            "ShaderBlob.h"
            "ShaderBlob.cpp"
    )
    target_include_directories(ShaderMakeBlob PUBLIC "${SHADERMAKE_ROOT}")
    target_compile_features(ShaderMakeBlob PUBLIC cxx_std_17)
    set_target_properties(ShaderMakeBlob PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND SHADERMAKE_TARGETS ShaderMakeBlob)
endif()

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(ShaderMake)
target_sources(ShaderMake
    PRIVATE
        "argparse.h"
        "argparse.c"
        "ShaderMake.cpp"
)
target_compile_features(ShaderMake PRIVATE c_std_99 cxx_std_17)
target_link_libraries(ShaderMake
    PRIVATE
        ShaderMakeBlob
        Threads::Threads
)

foreach(SHADERMAKE_TARGET IN LISTS SHADERMAKE_TARGETS)
    foreach(SHADERMAKE_LANGUAGE IN ITEMS C CXX)
        target_compile_options(${SHADERMAKE_TARGET}
            PRIVATE
                "$<$<COMPILE_LANG_AND_ID:${SHADERMAKE_LANGUAGE},Clang,AppleClang,GNU>:-Wextra>"
                "$<$<AND:$<COMPILE_LANGUAGE:${SHADERMAKE_LANGUAGE}>,$<${SHADERMAKE_LANGUAGE}_COMPILER_FRONTEND_VARIANT:MSVC>>:/W4;/WX>"
        )
    endforeach()
    target_compile_definitions(${SHADERMAKE_TARGET}
        PRIVATE
            "$<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN;NOMINMAX;_CRT_SECURE_NO_WARNINGS>"
    )
    set_target_properties(${SHADERMAKE_TARGET}
        PROPERTIES
            C_EXTENSIONS OFF
            CXX_EXTENSIONS OFF
            FOLDER "ShaderMake"
    )
endforeach()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
block(SCOPE_FOR VARIABLES)
    cmake_push_check_state(RESET)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(SHADERMAKE_FILESYSTEM_FOUND FALSE)

    # Older standard libraries need a separate filesystem library
    foreach(LIBRARY IN ITEMS native stdc++fs c++fs)
        set(CMAKE_REQUIRED_LIBRARIES "")
        if(NOT LIBRARY STREQUAL "native")
            set(CMAKE_REQUIRED_LIBRARIES "${LIBRARY}")
        endif()

        string(MAKE_C_IDENTIFIER "SHADERMAKE_FILESYSTEM_${LIBRARY}" CHECK)
        check_cxx_source_compiles("
            #include <filesystem>
            int main() { return std::filesystem::exists(\".\") ? 0 : 1; }
        " ${CHECK})
        if(${CHECK})
            if(NOT LIBRARY STREQUAL "native")
                target_link_libraries(ShaderMake PRIVATE "${LIBRARY}")
            endif()
            set(SHADERMAKE_FILESYSTEM_FOUND TRUE)
            break()
        endif()
    endforeach()

    cmake_pop_check_state()
    if(NOT SHADERMAKE_FILESYSTEM_FOUND)
        message(FATAL_ERROR "ShaderMake: a working C++17 filesystem library is required")
    endif()
endblock()

if(SHADERMAKE_BIN_OUTPUT_PATH)
    set_target_properties(ShaderMake
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SHADERMAKE_BIN_OUTPUT_PATH}/$<CONFIG>"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SHADERMAKE_BIN_OUTPUT_PATH}/Debug"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SHADERMAKE_BIN_OUTPUT_PATH}/MinSizeRel"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SHADERMAKE_BIN_OUTPUT_PATH}/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SHADERMAKE_BIN_OUTPUT_PATH}/RelWithDebInfo"
    )
endif()
