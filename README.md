# NVIDIA-RTX ShaderMake C++ Abstraction

The purpose of this library  to compile directly from you apps/engines instead of using ShaderMake.exe

DXIL Command:
```
ShaderMake.exe
    --platform DXIL
    --binary -O3
    -c "Shader.cfg"
    -o "bin"
    --compiler "%VULKAN_SDK%\Bin\dxc.exe" 
    --tRegShift 0 --sRegShift 128  --bRegShift 256 --uRegShift 384
    --useAPI
```

SPIRV Vulkan:
```
ShaderMake.exe --platform SPIRV --binary -O3
    -c "Shader.cfg"
    -o "bin"
    --compiler "%VULKAN_SDK%\Bin\dxc.exe"
    --tRegShift 0  --sRegShift 128  --bRegShift 256 --uRegShift 384
    --useAPI
    -D SPIRV
    -D TARGET_VULKAN
```

Usage example:
``` cpp
#define SHADERMAKE_COLORS
#include <ShaderMake/ShaderMake.h>

#include <assert.h>
#include <initializer_list>

using namespace ShaderMake;

#define COMPILE_DXIL 1

int main(int argc, char **argv)
{
    Options options;
    options.compilerType = CompilerType_DXC;
    options.optimizationLevel = 3;
    options.baseDirectory = "resources/shaders/";
    options.outputDir = "bin";

#if COMPILE_DXIL
    options.platformType = PlatformType_DXIL;
#else
    options.platformType = PlatformType_SPIRV;
#endif

    Context ctx(&options);

    ShaderContextDesc shaderDesc = ShaderContextDesc();
    bool forceRecompile = true;
    std::shared_ptr<ShaderContext> imguiVertexShader = std::make_shared<ShaderContext>("imgui.vertex.hlsl", ShaderType::Vertex, shaderDesc, forceRecompile);
    std::shared_ptr<ShaderContext> imguiPixelShader = std::make_shared<ShaderContext>("imgui.pixel.hlsl", ShaderType::Pixel, shaderDesc, forceRecompile);
    std::shared_ptr<ShaderContext> vertexShader = std::make_shared<ShaderContext>("test.vertex.hlsl", ShaderType::Vertex, shaderDesc, forceRecompile);
    std::shared_ptr<ShaderContext> pixelShader = std::make_shared<ShaderContext>("test.pixel.hlsl", ShaderType::Pixel, shaderDesc, forceRecompile);
    CompileStatus status = ctx.CompileOrGetShader({ imguiVertexShader, imguiPixelShader, vertexShader, pixelShader });

    // compile with .cfg file
    // TODO: get shader compilation result (blob)
#if 0
    ctx.CompileConfigFile("Shader.cfg");
#endif

    return (ctx.terminate || ctx.failedTaskCount > 0) ? 1 : 0;
}
```

ShaderMake is a frond-end tool for batch multi-threaded shader compilation developed by NVIDIA DevTech. It is compatible with Microsoft FXC and DXC compilers by calling them via API functions or executing them through command line, and with [Slang](https://github.com/shader-slang/slang) through command line only.

Features:

- Generates DXBC, DXIL and SPIR-V code.
- Outputs results in 3 formats: native binary, header file, and a [binary blob](#user-content-shader-blob-api) containing all permutations for a given shader.
- Minimizes the number of re-compilation tasks by tracking file modification times and include trees.

During project deployment, the *CMake* script automatically searches for `fxc` and `dxc` and sets these variables:

- `FXC_PATH` - `fxc` from *Windows SDK*
- `DXC_PATH` - `dxc` from *Windows SDK*
- `DXC_SPIRV_PATH` - `dxc` with enabled SPIRV generation from *Vulkan SDK*

## Command line options

Usage:

```
ShaderMake.exe -p {DXBC|DXIL|SPIRV} --binary [--header --blob] -c "path/to/config"
        -o "path/to/output" --compiler "path/to/compiler" [other options]
        -D DEF1 -D DEF2=1 ... -I "path1" -I "path2" ...

    -h, --help                show this help message and exit
```

Required options:
- `-p, --platform` (string) - DXBC, DXIL or SPIRV
- `-c, --config` (string) - Configuration file with the list of shaders to compile
- `-o, --out` (string) - Output directory
- `-b, --binary` - Output binary files
- `-h, --header` - Output header files
- `-B, --binaryBlob` - Output binary blob files
- `-H, --headerBlob` - Output header blob files
- `--compiler` (string) - Path to a FXC/DXC/Slang compiler

Compiler settings:
- `-m, --shaderModel` (string) - Shader model for DXIL/SPIRV (always SM 5.0 for DXBC) in 'X_Y' format
- `-O, --optimization` (int) - Optimization level 0-3 (default = 3, disabled = 0)
- `-X, --compilerOptions` (string) - Custom command line options for the compiler, separated by spaces
- `--WX` - Maps to '-WX' DXC/FXC option: warnings are errors
- `--allResourcesBound` - Maps to `-all_resources_bound` DXC/FXC option: all resources bound
- `--PDB` - Output PDB files in `out/PDB/` folder
- `--embedPDB`- Embed PDB with the shader binary
- `--stripReflection` - Maps to `-Qstrip_reflect` DXC/FXC option: strip reflection information from a shader binary
- `--matrixRowMajor` - Maps to `-Zpr` DXC/FXC option: pack matrices in row-major order
- `--hlsl2021` - Maps to `-HV 2021` DXC option: enable HLSL 2021 standard
- `--slang` - Use Slang for compilation, requires `--compiler` to specify a path to `slangc` executable
- `--slangHLSL` - Use HLSL compatibility mode when compiler is Slang

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
- `--useAPI` - Use *FXC (d3dcompiler)* or *DXC (dxcompiler)* API explicitly (Windows only)
- `--colorize` - Colorize console output
- `--verbose` - Print commands before they are executed
- `--retryCount` - Retry count for compilation task sub-process failures
- `--ignoreConfigDir` - Use 'current dir' instead of 'config dir' as parent path for relative dirs

SPIRV options:
- `--vulkanMemoryLayout` (string) - Maps to `-fvk-use-<VALUE>-layout` DXC options: dx, gl, scalar
- `--vulkanVersion` (string) - Vulkan environment version, maps to `-fspv-target-env` (default = 1.3)
- `--spirvExt` (string) - Maps to `-fspv-extension` option: add SPIR-V extension permitted to use
- `--sRegShift` (int) - SPIRV: register shift for sampler (`s#`) resources
- `--tRegShift` (int) - SPIRV: register shift for texture (`t#`) resources
- `--bRegShift` (int) - SPIRV: register shift for constant (`b#`) resources
- `--uRegShift` (int) - SPIRV: register shift for UAV (`u#`) resources
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
- `-m, --shaderModel` (string, optional) - Shader model for DXIL/SPIRV (always SM 5.0 for DXBC) in 'X_Y' format

Additionally, the config file parser supports:

- One line comments starting with `//`
- `#ifdef D`, where `D` is a macro definition name (the statement resolves to `true` if `D` is defined in the command line)
- `#if 1` and `#if 0`
- `#else`
- `#endif`

## Shader blob API

When the `--binaryBlob` or `--headerBlob` command line arguments are specified, ShaderMake will package multiple permutations for the same shader into a single "blob" file. These files use a custom format that is somewhat similar to regular TAR.

ShaderMake provides a small library with parsing functions to use these blob files in applications. This library can be statically linked with an application by including ShaderMake as a git submodule and linking the `ShaderMakeBlob` target to your application:

    target_link_libraries(my_target PRIVATE ShaderMakeBlob)

Then include `<ShaderMake/ShaderBlob.h>` and use the `ShaderMake::FindPermutationInBlob` to locate a specific shader version in a blob. If that is unsuccessful, the `ShaderMake::EnumeratePermutationsInBlob` and/or `ShaderMake::FormatShaderNotFoundMessage` functions can help you provide a helpful error message to the user.
