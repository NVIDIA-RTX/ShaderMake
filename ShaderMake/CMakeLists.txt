cmake_minimum_required(VERSION 3.15)

project(ShaderMake LANGUAGES C CXX)

# Globals?
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

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

# ShaderMake
add_executable(${PROJECT_NAME}
    "argparse.h"
    "argparse.c"
    "ShaderBlob.h"
    "ShaderBlob.cpp"
    "ShaderMake.cpp"
)
target_compile_options(${PROJECT_NAME} PRIVATE ${COMPILE_OPTIONS})

if(SHADERMAKE_BIN_OUTPUT_PATH)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SHADERMAKE_BIN_OUTPUT_PATH}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SHADERMAKE_BIN_OUTPUT_PATH}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SHADERMAKE_BIN_OUTPUT_PATH}/MinSizeRel"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SHADERMAKE_BIN_OUTPUT_PATH}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SHADERMAKE_BIN_OUTPUT_PATH}/RelWithDebInfo"
    )
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY FOLDER "ShaderMake")

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(APPLE)
    target_link_libraries(${PROJECT_NAME} pthread)
else()
    target_link_libraries(${PROJECT_NAME} stdc++fs pthread)
endif()
