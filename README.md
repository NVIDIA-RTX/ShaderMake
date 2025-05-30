# ShaderMake

[![Build Status](https://github.com/NVIDIA-RTX/ShaderMake/actions/workflows/build.yml/badge.svg)](https://github.com/NVIDIA-RTX/ShaderMake/actions/workflows/build.yml)

ShaderMake is a frond-end tool for batch multi-threaded shader compilation developed by NVIDIA DevTech. It is compatible with Microsoft *FXC* and *DXC* compilers by calling them via API functions or executing them through command line, and with [Slang](https://github.com/shader-slang/slang) through command line only.

Features:

- Generates *DXBC*, *DXIL* *SPIRV* and any produced by *Slang*;
- Output formats: a native binary, a header file, and a binary or header [blob](#user-content-shader-blob) (containing all permutations for a givan input shader file);
- Minimizes the number of re-compilation tasks by tracking file modification times and include trees.

*CMake* options:

- `SHADERMAKE_FIND_FXC` - find *FXC* in installed [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) and populate `SHADERMAKE_FXC_PATH`
- `SHADERMAKE_FIND_DXC` -  download [DXC](https://github.com/microsoft/DirectXShaderCompiler) from *GitHub* and populate `SHADERMAKE_DXC_PATH`
- `SHADERMAKE_FIND_DXC_VK` - find *DXC* in installed [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) and populate `SHADERMAKE_DXC_VK_PATH` (this is the only way to get *DXC* on *MacOS* currently)
- `SHADERMAKE_FIND_SLANG` - download [Slang](https://github.com/shader-slang/slang) from *GitHub* and populate `SHADERMAKE_SLANG_PATH`
- `SHADERMAKE_FIND_COMPILERS` - master switch
- `SHADERMAKE_DXC_VERSION` - *DXC* to download from *GitHub/DirectXShaderCompiler* releases
- `SHADERMAKE_DXC_DATE` - *DXC* release date (unfortunately present in the download links)
- `SHADERMAKE_SLANG_VERSION` - *Slang* to download from *GitHub/Shader-slang/slang* releases
- `SHADERMAKE_TOOL` - Use ShaderMake as an external tool and hide the executable from the parent project IDE. It's especially handy to avoid shader recompilation on switching build configurations

If one of `SHADERMAKE_DXC_PATH` or `SHADERMAKE_DXC_VK_PATH` left empty during deployment, it will be set to the valid one (both support *DXIL* and *SPIRV* code generation). After deployment `SHADERMAKE_PATH` CMake variable stores the path to the ShaderMake executable.

## Command line options

Usage:

```
ShaderMake.exe -p {DXBC|DXIL|SPIRV} --binary [--header --blob] -c "path/to/config"
        -o "path/to/output" --compiler "path/to/compiler" [other options]
        -D DEF1 -D DEF2=1 ... -I "path1" -I "path2" ...

    -h, --help                show this help message and exit
```

Required options:
- `-p, --platform` (string) - *DXBC*, *DXIL* or *SPIRV*
- `-c, --config` (string) - Configuration file with the list of shaders to compile
- `-o, --out` (string) - Output directory
- `-b, --binary` - Output binary files
- `-h, --header` - Output header files
- `-B, --binaryBlob` - Output binary blob files
- `-H, --headerBlob` - Output header blob files
- `--compiler` (string) - Path to a *FXC/DXC/Slang* compiler

Compiler settings:
- `-m, --shaderModel` (string) - Shader model for *DXIL/SPIRV* (always SM 5.0 for *DXBC*) in 'X_Y' format
- `-O, --optimization` (int) - Optimization level 0-3 (default = 3, disabled = 0)
- `-X, --compilerOptions` (string) - Custom command line options for the compiler, separated by spaces
- `--WX` - Maps to '-WX' *DXC/FXC* option: warnings are errors
- `--allResourcesBound` - Maps to `-all_resources_bound` *DXC/FXC* option: all resources bound
- `--PDB` - Output PDB files in `out/PDB/` folder
- `--embedPDB`- Embed PDB with the shader binary
- `--stripReflection` - Maps to `-Qstrip_reflect` *DXC/FXC* option: strip reflection information from a shader binary
- `--matrixRowMajor` - Maps to `-Zpr` *DXC/FXC* option: pack matrices in row-major order
- `--hlsl2021` - Maps to `-HV 2021` *DXC* option: enable HLSL 2021 standard
- `--slang` - Use *Slang* for compilation, requires `--compiler` to specify a path to `slangc` executable
- `--slangHLSL` - Use HLSL compatibility mode when compiler is *Slang*

Defines & include directories:
- `-I, --include` (string) - Include directory(s)
- `-D, --define` (string) - Macro definition(s) in forms 'M=value' or 'M'

Other options:
- `-f, --force` - Treat all source files as modified
- `--sourceDir` (string) - Source code directory
- `--relaxedInclude` (string) - Include file(s) not invoking re-compilation
- `--outputExt` (string) - Extension for output files, default is one of `.dxbc`, `.dxil`, `.spirv`
- `--serial` - Disable multi-threading
- `--flatten` - Flatten source directory structure in the output directory
- `--continue` - Continue compilation if an error is occured
- `--colorize` - Colorize console output
- `--verbose` - Print commands before they are executed
- `--retryCount` - Retry count for compilation task sub-process failures
- `--ignoreConfigDir` - Use 'current dir' instead of 'config dir' as parent path for relative dirs
- `--compactProgress` - Compact compilation progress reporting

*SPIRV* options:
- `--vulkanMemoryLayout` (string) - Maps to `-fvk-use-<VALUE>-layout` *DXC* options: dx, gl, scalar
- `--vulkanVersion` (string) - Vulkan environment version, maps to `-fspv-target-env` (default = 1.3)
- `--spirvExt` (string) - Maps to `-fspv-extension` option: add *SPIRV* extension permitted to use
- `--sRegShift` (int) - register shift for sampler (`s#`) resources
- `--tRegShift` (int) - register shift for texture (`t#`) resources
- `--bRegShift` (int) - register shift for constant (`b#`) resources
- `--uRegShift` (int) - register shift for UAV (`u#`) resources
- `--noRegShifts` - Don't specify any register shifts for the compiler

## Config file structure

A config file consists of several lines, where each line has the following structure:

```
path/to/shader -T profile [-O3 -o "output/subdirectory" -E entry -D DEF1={0,1} -D DEF2={0,1,2} -D DEF3]
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
- `-E, --entryPoint` (string, optional) - Entry point (`main` by default)
- `-D, --define` (string, optional) - Adds a macro definition to the list, optional range of possible values can be provided in `{}`
- `-O, --optimization` (int, optional) - Optimization level (global setting used by default)
- `-o, --output` (string, optional) - Output directory override
- `-s, --outputSuffix` (string, optional) - Suffix to add before extension after filename
- `-m, --shaderModel` (string, optional) - Shader model for *DXIL/SPIRV* (always SM 5.0 for *DXBC*) in 'X_Y' format

Additionally, the config file parser supports:

- One line comments starting with `//`
- `#ifdef D`, where `D` is a macro definition name (the statement resolves to `true` if `D` is defined in the command line)
- `#if 1` and `#if 0`
- `#else`
- `#endif`

## Shader blob

When the `--binaryBlob` or `--headerBlob` command line arguments are specified, ShaderMake will package multiple permutations for the same shader into a single "blob" file with a custom format. ShaderMake provides a small library with parsing functions to use these blob files. This library can be statically linked with an application by including `ShaderMake` into the project and linking `ShaderMakeBlob` target to your application. Then include `<ShaderMake/ShaderBlob.h>` and use the `ShaderMake::FindPermutationInBlob()` to locate a specific shader permutation in a blob. If that is unsuccessful, `ShaderMake::EnumeratePermutationsInBlob()` and/or `ShaderMake::FormatShaderNotFoundMessage()` functions can help to provide a meaningful error message to the user.
