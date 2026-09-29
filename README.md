# ShaderMake

[![Build Status](https://github.com/NVIDIA-RTX/ShaderMake/actions/workflows/build.yml/badge.svg)](https://github.com/NVIDIA-RTX/ShaderMake/actions/workflows/build.yml)

ShaderMake is a front-end tool for batch multi-threaded shader compilation developed by NVIDIA DevTech. It is compatible with Microsoft *FXC* and *DXC* compilers by calling them via API functions or executing them through command line, and with [Slang](https://github.com/shader-slang/slang) through command line only.

Features:

- Generates *DXBC*, *DXIL* and *SPIR-V* using *FXC*, *DXC* or *Slang* where supported;
- Output formats: a native binary, a header file, and a binary or header [blob](#user-content-shader-blob) (containing all permutations for a given input shader file);
- Minimizes the number of re-compilation tasks by tracking file modification times, include trees, compiler executables and recognized runtime libraries, and global code-generation settings.

*CMake* options:

- `SHADERMAKE_FIND_FXC` - find *FXC* in installed [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) and populate `SHADERMAKE_FXC_PATH`
- `SHADERMAKE_FIND_DXC` -  download [DXC](https://github.com/microsoft/DirectXShaderCompiler) from *GitHub* and populate `SHADERMAKE_DXC_PATH`
- `SHADERMAKE_FIND_DXC_VK` - find *DXC* in installed [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) and populate `SHADERMAKE_DXC_VK_PATH` (this is the only way to get *DXC* on *macOS* currently)
- `SHADERMAKE_FIND_SLANG` - download [Slang](https://github.com/shader-slang/slang) from *GitHub* and populate `SHADERMAKE_SLANG_PATH`
- `SHADERMAKE_FIND_COMPILERS` - master switch
- `SHADERMAKE_DXC_VERSION` - *DXC* to download from *GitHub/DirectXShaderCompiler* releases
- `SHADERMAKE_DXC_DATE` - *DXC* release date (unfortunately present in the download links)
- `SHADERMAKE_SLANG_VERSION` - *Slang* to download from *GitHub/Shader-slang/slang* releases
- `SHADERMAKE_BIN_OUTPUT_PATH` - output directory for the ShaderMake executable
- `SHADERMAKE_TOOL` - use ShaderMake as an external tool and hide the executable from the parent project IDE; this avoids shader recompilation when switching build configurations

If either `SHADERMAKE_DXC_PATH` or `SHADERMAKE_DXC_VK_PATH` is empty during deployment, it is set to the available path because both compilers support *DXIL* and *SPIR-V* generation. After deployment, the `SHADERMAKE_PATH` CMake variable stores the path to the ShaderMake executable.

## Command line options

Usage:

```
ShaderMake.exe -p {DXBC|DXIL|SPIRV} [-b] [-h] [-B] [-H] -c "path/to/config"
        -o "path/to/output" --compiler "path/to/compiler" [--compilerAlias "NAME=path"] [other options]
        -D DEF1 -D DEF2=1 ... -I "path1" -I "path2" ...

    -h, --help                show this help message and exit
```

Required inputs and output selection:
- `-p, --platform` (string) - *DXBC*, *DXIL* or *SPIRV*
- `-c, --config` (string) - Configuration file with the list of shaders to compile
- `-o, --out` (string) - Output directory
- `-b, --binary` - Output binary files
- `-h, --header` - Output header files
- `-B, --binaryBlob` - Output binary blob files
- `-H, --headerBlob` - Output header blob files
- `--compiler` (string) - Path to an *FXC/DXC/Slang* compiler. An executable named `slangc` is detected automatically

Compiler settings:
- `--compilerAlias` (string) - Register `DXC` or `SLANG` as `NAME=path` for selection by config-local `--compilerDXIL` and `--compilerSPIRV`; names are case-insensitive and each alias may be registered once
- `-m, --shaderModel` (string) - Shader model for *DXIL/SPIRV* (always SM 5.0 for *DXBC*) in 'X_Y' format
- `-O, --optimization` (int) - Optimization level 0-3 (default = 3, disabled = 0)
- `-X, --compilerOptions` (string) - Custom command line options for the compiler, separated by spaces
- `--WX` - Treat warnings as errors (`-WX` for *DXC/FXC*, `-warnings-as-errors all` for *Slang*)
- `--allResourcesBound` - Maps to `-all_resources_bound` *DXC/FXC* option: all resources bound
- `--PDB` - Output PDB files in `out/PDB/`; ShaderMake does not currently support separate PDB output with Slang, so debug information remains in the shader binary
- `--embedPDB` - Embed PDB with the shader binary
- `--stripReflection` - Maps to `-Qstrip_reflect` *DXC/FXC* option: strip reflection information from a shader binary
- `--matrixRowMajor` - Maps to `-Zpr` *DXC/FXC* option: pack matrices in row-major order
- `--hlsl2021` - Maps to `-HV 2021` *DXC* option: enable HLSL 2021 standard
- `--slang` - Explicitly select *Slang* for wrappers or non-standard executable names
- `--slangHLSL` - Use HLSL compatibility mode when compiler is *Slang*

Defines & include directories:
- `-I, --include` (string) - Include directory(s)
- `-D, --define` (string) - Macro definition(s) in forms 'M=value' or 'M'

Other options:
- `-f, --force` - Treat all source files as modified
- `--project` (string) - Project name used in informational output
- `--sourceDir` (string) - Source code directory
- `--relaxedInclude` (string) - Include file(s) not invoking re-compilation
- `--outputExt` (string) - Extension for output files, default is one of `.dxbc`, `.dxil`, `.spirv`
- `--serial` - Disable multi-threading
- `-j, --jobs` (int) - Maximum number of parallel compilation tasks; `0` uses the number of logical processors (default = `0`)
- `--flatten` - Flatten source directory structure in the output directory
- `--continue` - Continue compilation if an error occurred
- `--colorize` - Colorize console output
- `--verbose` - Print commands before they are executed
- `--retryCount` (int) - Retry count for compilation task subprocess failures (default = 10)
- `--ignoreConfigDir` - Use 'current dir' instead of 'config dir' as parent path for relative dirs
- `--compactProgress` - Compact compilation progress reporting

*SPIR-V* options:
- `--vulkanMemoryLayout` (string) - Vulkan memory layout: dx, gl or scalar
- `--vulkanVersion` (string) - Vulkan environment version; maps to `-fspv-target-env` for *DXC*. For *Slang*, `1.0`, `1.1`, `1.1spirv1.4`, `1.2`, `1.3` and `1.4` map to `spirv_1_0`, `spirv_1_3`, `spirv_1_4`, `spirv_1_5`, `spirv_1_6` and `spirv_1_6`, respectively (default = `1.3`). Slang HLSL profiles impose minimum SPIR-V versions: SM 5.x requires 1.0, SM 6.0-6.2 requires 1.3, SM 6.3-6.7 requires 1.4, SM 6.8 and later SM 6.x require 1.5, and newer major models conservatively require 1.6; incompatible combinations are rejected
- `--spirvExt` (string) - Add a permitted *SPIR-V* extension (`-fspv-extension` for *DXC*, `-capability` for *Slang*); the legacy `KHR` wildcard is *DXC*-only and is omitted for *Slang*
- `--sRegShift` (int) - register shift for sampler (`s#`) resources
- `--tRegShift` (int) - register shift for texture (`t#`) resources
- `--bRegShift` (int) - register shift for constant (`b#`) resources
- `--uRegShift` (int) - register shift for UAV (`u#`) resources
- `--noRegShifts` - Don't specify any register shifts for the compiler

ShaderMake makes implicit definitions available in config files and shader sources, matching real compiler definitions:

- `__SLANG__` matches the definition exposed by *Slang* and is added when that compiler is selected
- `__spirv__` matches the *DXC* SPIR-V target definition and is added for every *SPIR-V* compiler

When *Slang* is selected, ShaderMake translates these known *DXC* SPIR-V options to their *Slang* equivalents:

- `-fspv-extension=SPV_EXT_mesh_shader` is omitted because the mesh-shader profile enables the extension automatically
- `-fspv-extension=SPV_*` maps to `-capability SPV_*`
- `-fspv-use-descriptor-heap` maps to `-capability spvDescriptorHeapEXT -spirv-unified-descriptor-heap-stride`
- `-fspv-use-unknown-image-format` maps to `-default-image-format-unknown`

Options without a known mapping are passed through unchanged.

For *DXC*, `-fspv-use-descriptor-heap` also permits `SPV_EXT_descriptor_heap` if it was not specified explicitly.

## Config file structure

A config file consists of several lines, where each line has the following structure:

```
path/to/shader -T profile [-O3] [-o "output/subdirectory"] [-E entry] [--compilerDXIL NAME] [--compilerSPIRV NAME] [-D DEF1={0,1}] [-D DEF2={0,1,2}] [-D DEF3]
```

where:
- `path/to/shader` (string) - shader source file
- `-T, --profile` (string) - shader profile, can be:
  - `vs` - vertex
  - `ps` - pixel
  - `gs` - geometry
  - `hs` - hull
  - `ds` - domain
  - `cs` - compute
  - `ms` - mesh
  - `as` - amplification
  - `lib` - library
- `-E, --entryPoint` (string, optional) - Entry point (`main` by default)
- `-D, --define` (string, optional) - Adds a macro definition to the list, optional range of possible values can be provided in `{}`
- `-O, --optimization` (int, optional) - Optimization level (global setting used by default)
- `-o, --output` (string, optional) - Output directory override
- `-s, --outputSuffix` (string, optional) - Suffix inserted before the first `.` in the filename (`Shader.cs.hlsl -s Test` produces `ShaderTest.cs.hlsl`)
- `-m, --shaderModel` (string, optional) - Shader model for *DXIL/SPIRV* (always SM 5.0 for *DXBC*) in 'X_Y' format
- `--compilerDXIL` (string, optional) - Select a compiler registered with the global `--compilerAlias NAME=path` option when compiling *DXIL*; ignored for other platforms
- `--compilerSPIRV` (string, optional) - Select a compiler registered with the global `--compilerAlias NAME=path` option when compiling *SPIR-V*; ignored for other platforms
- `-X, --compilerOptions` (string, optional) - Custom compiler options for this shader
- `--compilerOptionsDXIL` (string, optional) - Custom compiler options used only for *DXIL*
- `--compilerOptionsSPIRV` (string, optional) - Custom compiler options used only for *SPIR-V*
- `--noRegShifts` (optional) - Don't specify *SPIR-V* register shifts for this shader

Additionally, the config file parser supports:

- One line comments starting with `//`
- `#ifdef D`, where `D` is a global or implicit macro definition name
- `#if 1` and `#if 0`
- `#else`
- `#endif`

Config preprocessing uses the default compiler selected by the global `--compiler` option. Consequently, `#ifdef __SLANG__` reflects only the default compiler; config-local `--compilerDXIL` and `--compilerSPIRV` overrides define `__SLANG__` for the shader source but do not affect config preprocessing.

Every active shader permutation must produce a unique output path. ShaderMake rejects duplicate outputs because compiling them concurrently would be unsafe.

### Output file naming

ShaderMake inserts every generated filename suffix before the first extension that remains after removing the source extension. This applies to the explicit `--outputSuffix`, a non-`main` entry point, and the permutation hash. The platform extension is appended afterward.

For example, compiling `Shader.cs.hlsl` for *DXIL* with `-s _Test -E CSMain` and permutation defines produces names in this form:

```text
Shader_Test_CSMain_0123ABCD.cs.dxil
```

Header output adds `.h` after the platform extension, while blob output follows the same suffix placement for its base filename.

## Shader blob

When the `--binaryBlob` or `--headerBlob` command line arguments are specified, ShaderMake will package multiple permutations for the same shader into a single "blob" file with a custom format. ShaderMake provides a small library with parsing functions to use these blob files. This library can be statically linked with an application by including `ShaderMake` into the project and linking `ShaderMakeBlob` target to your application. Then include `<ShaderMake/ShaderBlob.h>` and use the `ShaderMake::FindPermutationInBlob()` to locate a specific shader permutation in a blob. If that is unsuccessful, `ShaderMake::EnumeratePermutationsInBlob()` and/or `ShaderMake::FormatShaderNotFoundMessage()` functions can help to provide a meaningful error message to the user.
