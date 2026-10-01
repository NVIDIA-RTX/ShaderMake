/*
Copyright (c) 2014-2025, NVIDIA CORPORATION. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "ShaderBlob.h"
#include "argparse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <limits.h>
#    include <sys/wait.h>
#    include <time.h>
#    include <unistd.h>
#endif

using namespace std;
namespace fs = filesystem;

#define _L(x)       __L(x)
#define __L(x)      L##x
#define UNUSED(x)   ((void)(x))
#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

#define USE_GLOBAL_OPTIMIZATION_LEVEL 0xFF
#define SPIRV_SPACES_NUM              8
#define PDB_DIR                       "PDB"
#define BUF_SIZE                      2048 // max line length
#define DEVMODE                       1    // 1 forces recompilation if EXE changes

#ifdef _MSC_VER
#    define popen  _popen
#    define pclose _pclose
#endif

enum Platform : uint8_t {
    DXBC,
    DXIL,
    SPIRV,
    METAL,

    PLATFORMS_NUM
};

enum CompilerType : uint8_t {
    COMPILER_FXC,
    COMPILER_DXC,
    COMPILER_SLANG,
    COMPILER_METAL,
    COMPILER_METAL_SHADER_CONVERTER,
};

struct CompilerAlias {
    string name;
    fs::path path;
    CompilerType type = COMPILER_DXC;
};

struct Options {
    vector<fs::path> includeDirs;
    vector<fs::path> relaxedIncludes;
    vector<string> defines;
    vector<string> spirvExtensions = {"SPV_EXT_descriptor_indexing", "KHR"};
    vector<string> compilerOptions;
    vector<string> metalShaderConverterOptions;
    vector<string> compilerAliasArgs;
    vector<CompilerAlias> compilerAliases;
    fs::path configFile;
    fs::path metalCompiler;
    fs::path metalShaderConverter;
    fs::path sourceDir;
    const char* projectName = "";
    const char* platformName = nullptr;
    const char* outputDir = nullptr;
    const char* shaderModel = "6_5";
    const char* vulkanVersion = "1.3";
    const char* compiler = nullptr;
    const char* outputExt = nullptr;
    const char* vulkanMemoryLayout = nullptr;
    const char* metalSdk = "macosx";
    const char* metalStd = "metal4.0";
    const char* metalMinOS = "26.0";
    uint32_t sRegShift = 100;
    uint32_t tRegShift = 200;
    uint32_t bRegShift = 300;
    uint32_t uRegShift = 400;
    uint32_t optimizationLevel = 3;
    int32_t retryCount = 10; // default 10 retries for compilation task sub-process failures
    int32_t jobs = 0;
    Platform platform = DXBC;
    CompilerType compilerType = COMPILER_DXC;
    bool serial = false;
    bool flatten = false;
    bool force = false;
    bool help = false;
    bool binary = false;
    bool header = false;
    bool binaryBlob = false;
    bool headerBlob = false;
    bool continueOnError = false;
    bool warningsAreErrors = false;
    bool allResourcesBound = false;
    bool pdb = false;
    bool embedPdb = false;
    bool stripReflection = false;
    bool matrixRowMajor = false;
    bool hlsl2021 = false;
    bool verbose = false;
    bool colorize = false;
    bool slang = false;
    bool slangHlsl = false;
    bool noRegShifts = false;
    bool compactProgress = false;
    bool metalFromDxil = false;

    bool Parse(int32_t argc, const char** argv);

    bool IsBlob() const {
        return binaryBlob || headerBlob;
    }
};

struct ConfigLine {
    vector<string> defines;
    vector<string> compilerOptions;
    vector<string> compilerOptionsDXIL;
    vector<string> compilerOptionsSPIRV;
    vector<string> metalShaderConverterOptions;
    const char* source = nullptr;
    const char* entryPoint = "main";
    const char* profile = nullptr;
    const char* outputDir = nullptr;
    const char* outputSuffix = nullptr;
    const char* shaderModel = nullptr;
    const char* compilerDXIL = nullptr;
    const char* compilerSPIRV = nullptr;

    uint32_t optimizationLevel = USE_GLOBAL_OPTIMIZATION_LEVEL;
    bool noRegShifts = false;

    bool Parse(int32_t argc, const char** argv);
};

struct TaskData {
    vector<string> defines;
    vector<string> compilerOptions;
    vector<string> compilerOptionsDXIL;
    vector<string> compilerOptionsSPIRV;
    vector<string> metalShaderConverterOptions;
    string source;
    string entryPoint;
    string profile;
    string shaderModel;
    string compiler;
    string outputFileWithoutExt;
    string combinedDefines;
    uint32_t optimizationLevel = 3;
    CompilerType compilerType = COMPILER_DXC;
    bool noRegShifts = false;
};

struct BlobEntry {
    string permutationFileWithoutExt;
    string combinedDefines;
};

struct BuildSignature {
    fs::path path;
    string value;
    bool changed = false;
};

Options g_Options;
map<fs::path, fs::file_time_type> g_HierarchicalUpdateTimes;
map<string, vector<BlobEntry>> g_ShaderBlobs;
map<string, uint32_t> g_OutputLines;
map<string, BuildSignature> g_CompilerAliasBuildSignatures;
vector<TaskData> g_TaskData;
mutex g_TaskMutex;
mutex g_ProgressMutex;
atomic<uint32_t> g_ProcessedTaskCount;
uint32_t g_PrevProgress;
atomic<int32_t> g_TaskRetryCount;
atomic<bool> g_Terminate = false;
atomic<uint32_t> g_FailedTaskCount = 0;
uint32_t g_OriginalTaskCount;
const char* g_OutputExt = nullptr;

std::array<const char*, PLATFORMS_NUM> g_PlatformNames = {
    "DXBC",
    "DXIL",
    "SPIRV",
    "METAL",
};

std::array<const char*, PLATFORMS_NUM> g_PlatformExts = {
    ".dxbc",
    ".dxil",
    ".spirv",
    ".metallib",
};

// Metal converter bundle ("ShaderBlob.h")
const char* g_MetalConverterBundleExt = ".metalbundle";

std::array<const char*, 3> g_PlatformSlangTargets = {
    "dxbc",
    "dxil",
    "spirv",
};

#if 1
#    define RED    "\x1b[31m"
#    define GRAY   "\x1b[90m"
#    define WHITE  "\x1b[0m"
#    define GREEN  "\x1b[32m"
#    define YELLOW "\x1b[33m"
#else
#    define RED    ""
#    define GRAY   ""
#    define WHITE  ""
#    define GREEN  ""
#    define YELLOW ""
#endif

/*
Naming convention:
    - file        - "path/to/name{.ext}"
    - name        - "name" from "file"
    - path        - "path" from "file"
    - permutation - "name" + "hash"
    - stream      - FILE or ifstream
*/

//=====================================================================================================================
// MISC
//=====================================================================================================================

static inline uint32_t HashToUint(size_t hash) {
    return uint32_t(hash) ^ (uint32_t(hash >> 32));
}

static inline void InsertSuffixBeforeExtensions(fs::path& path, const string& suffix) {
    string filename = path.filename().string();
    const size_t extensionPos = filename.find('.');
    filename.insert(extensionPos == string::npos ? filename.size() : extensionPos, suffix);
    path = path.parent_path() / filename;
}

static uint64_t HashString(const string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }

    return hash;
}

static string PathToString(fs::path path) {
    return path.lexically_normal().make_preferred().string();
}

static fs::path RemoveLeadingDotDots(const fs::path& path) {
    auto it = path.begin();
    while (it != path.end() && *it == "..")
        ++it;

    fs::path result;
    while (it != path.end()) {
        result = result / *it;
        ++it;
    }

    return result;
}

static inline bool IsSpace(char ch) {
    return strchr(" \t\r\n", ch) != nullptr;
}

static bool IsSlangCompiler(const char* compiler) {
    string name = fs::path(compiler).filename().string();
    transform(name.begin(), name.end(), name.begin(), [](char ch) { return (char)tolower((unsigned char)ch); });

    return name == "slangc" || name == "slangc.exe";
}

static string ToUpper(string value) {
    transform(value.begin(), value.end(), value.begin(), [](char ch) { return (char)toupper((unsigned char)ch); });

    return value;
}

static bool GetCompilerType(const string& name, CompilerType& type) {
    if (name == "DXC")
        type = COMPILER_DXC;
    else if (name == "SLANG")
        type = COMPILER_SLANG;
    else
        return false;

    return true;
}

static const CompilerAlias* FindCompilerAlias(const char* name) {
    string normalizedName = ToUpper(name);
    for (const CompilerAlias& compilerAlias : g_Options.compilerAliases) {
        if (compilerAlias.name == normalizedName)
            return &compilerAlias;
    }

    return nullptr;
}

static bool HasDefine(const vector<string>& defines, const char* name) {
    size_t nameLength = strlen(name);
    for (const string& define : defines) {
        if (define.compare(0, nameLength, name) == 0 && (define.size() == nameLength || define[nameLength] == '='))
            return true;
    }

    return false;
}

static void AddImplicitDefine(vector<string>& defines, const char* name) {
    if (!HasDefine(defines, name))
        defines.push_back(name);
}

static bool HasConfigDefine(const char* name) {
    if (HasDefine(g_Options.defines, name))
        return true;
    if (strcmp(name, "__SLANG__") == 0)
        return g_Options.compilerType == COMPILER_SLANG;
    if (strcmp(name, "__spirv__") == 0)
        return g_Options.platform == SPIRV;
    if (strcmp(name, "__METAL__") == 0)
        return g_Options.platform == METAL;

    return false;
}

static inline bool IsCompilerOptionBoundary(const string& options, size_t pos) {
    return pos == options.size() || IsSpace(options[pos]);
}

static void ReplaceCompilerOption(string& options, const char* from, const char* to) {
    size_t fromLength = strlen(from);
    size_t pos = 0;
    while ((pos = options.find(from, pos)) != string::npos) {
        bool isStart = pos == 0 || IsSpace(options[pos - 1]);
        bool isEnd = IsCompilerOptionBoundary(options, pos + fromLength);
        if (isStart && isEnd) {
            options.replace(pos, fromLength, to);
            pos += strlen(to);
        } else
            pos += fromLength;
    }
}

static bool HasCompilerOption(const string& options, const char* option) {
    size_t optionLength = strlen(option);
    size_t pos = 0;
    while ((pos = options.find(option, pos)) != string::npos) {
        bool isStart = pos == 0 || IsSpace(options[pos - 1]);
        bool isEnd = IsCompilerOptionBoundary(options, pos + optionLength);
        if (isStart && isEnd)
            return true;

        pos += optionLength;
    }

    return false;
}

static void TranslateSlangSpirvExtensions(string& options) {
    static const char* prefix = "-fspv-extension=";
    const size_t prefixLength = strlen(prefix);
    size_t pos = 0;
    while ((pos = options.find(prefix, pos)) != string::npos) {
        if (pos != 0 && !IsSpace(options[pos - 1])) {
            pos += prefixLength;
            continue;
        }

        size_t extensionPos = pos + prefixLength;
        size_t end = options.find_first_of(" \t\r\n", extensionPos);
        string extension = options.substr(extensionPos, end - extensionPos);
        if (extension == "SPV_EXT_mesh_shader")
            options.erase(pos, end - pos);
        else if (extension.compare(0, 4, "SPV_") == 0) {
            string replacement = "-capability " + extension;
            options.replace(pos, end - pos, replacement);
            pos += replacement.size();
        } else
            pos = end;
    }
}

static string TranslateCompilerOptions(const string& options, CompilerType compilerType) {
    if (g_Options.platform != SPIRV)
        return options;

    string translatedOptions = options;
    if (compilerType == COMPILER_SLANG) {
        TranslateSlangSpirvExtensions(translatedOptions);
        ReplaceCompilerOption(translatedOptions, "-fspv-use-descriptor-heap", "-capability spvDescriptorHeapEXT -spirv-unified-descriptor-heap-stride");
        ReplaceCompilerOption(translatedOptions, "-fspv-use-unknown-image-format", "-default-image-format-unknown");
    } else if (HasCompilerOption(translatedOptions, "-fspv-use-descriptor-heap") && !HasCompilerOption(translatedOptions, "-fspv-extension=SPV_EXT_descriptor_heap")) {
        translatedOptions += " -fspv-extension=SPV_EXT_descriptor_heap";
    }

    return translatedOptions;
}

static void AppendCompilerOptions(ostringstream& cmd, const vector<string>& compilerOptions, CompilerType compilerType) {
    for (const string& options : compilerOptions) {
        string translatedOptions = TranslateCompilerOptions(options, compilerType);
        if (translatedOptions.find_first_not_of(" \t\r\n") != string::npos)
            cmd << " " << translatedOptions;
    }
}

static const char* GetSlangSpirvCapability(const char* vulkanVersion) {
    if (strcmp(vulkanVersion, "1.0") == 0)
        return "spirv_1_0";
    if (strcmp(vulkanVersion, "1.1") == 0)
        return "spirv_1_3";
    if (strcmp(vulkanVersion, "1.1spirv1.4") == 0)
        return "spirv_1_4";
    if (strcmp(vulkanVersion, "1.2") == 0)
        return "spirv_1_5";
    if (strcmp(vulkanVersion, "1.3") == 0 || strcmp(vulkanVersion, "1.4") == 0)
        return "spirv_1_6";

    return nullptr;
}

static void AppendBuildSignatureValue(ostringstream& signature, const char* name, const string& value) {
    signature << name << '=' << value.size() << ':' << value << '\n';
}

static void AppendBuildSignatureValues(ostringstream& signature, const char* name, const vector<string>& values) {
    signature << name << ".size=" << values.size() << '\n';
    for (const string& value : values)
        AppendBuildSignatureValue(signature, name, value);
}

static void AppendBuildSignaturePaths(ostringstream& signature, const char* name, const vector<fs::path>& paths) {
    signature << name << ".size=" << paths.size() << '\n';
    for (const fs::path& path : paths)
        AppendBuildSignatureValue(signature, name, PathToString(path));
}

static string FileTimeToString(fs::file_time_type time) {
    // libc++ can use a 128-bit filesystem clock, which has no to_string overload.
    auto ticks = time.time_since_epoch().count();
    const bool negative = ticks < 0;
    string result;
    do {
        const int digit = static_cast<int>(ticks % 10);
        result.push_back(static_cast<char>('0' + (digit < 0 ? -digit : digit)));
        ticks /= 10;
    } while (ticks != 0);
    if (negative)
        result.push_back('-');
    reverse(result.begin(), result.end());

    return result;
}

static void AppendCompilerBuildSignature(ostringstream& signature, const fs::path& compilerPath, CompilerType compilerType) {
    fs::path path = fs::absolute(compilerPath).lexically_normal();
    AppendBuildSignatureValue(signature, "compiler", PathToString(path));
    AppendBuildSignatureValue(signature, "compilerType", to_string(compilerType));
    AppendBuildSignatureValue(signature, "compilerSize", to_string(fs::file_size(path)));
    AppendBuildSignatureValue(signature, "compilerTime", FileTimeToString(fs::last_write_time(path)));

#ifdef _WIN32
    static const char* dxcSidecars[] = {"dxcompiler.dll", "dxil.dll"};
    static const char* slangSidecars[] = {
        "gfx.dll", "slang-compiler.dll", "slang-glsl-module.dll", "slang-glslang.dll",
        "slang-llvm.dll", "slang-rt.dll", "slang.dll"};
    static const char* metalShaderConverterSidecars[] = {"metalirconverter.dll"};
#elif defined(__APPLE__)
    static const char* dxcSidecars[] = {"libdxcompiler.dylib", "libdxil.dylib"};
    static const char* slangSidecars[] = {
        "libgfx.dylib", "libslang-compiler.dylib", "libslang-glsl-module.dylib", "libslang-glslang.dylib",
        "libslang-llvm.dylib", "libslang-rt.dylib", "libslang.dylib"};
    static const char* metalShaderConverterSidecars[] = {"libmetalirconverter.dylib"};
#else
    static const char* dxcSidecars[] = {"libdxcompiler.so", "libdxil.so"};
    static const char* slangSidecars[] = {
        "libgfx.so", "libslang-compiler.so", "libslang-glsl-module.so", "libslang-glslang.so",
        "libslang-llvm.so", "libslang-rt.so", "libslang.so"};
    static const char* metalShaderConverterSidecars[] = {"libmetalirconverter.so"};
#endif

    const char* const* sidecars = nullptr;
    size_t sidecarNum = 0;
    if (compilerType == COMPILER_DXC) {
        sidecars = dxcSidecars;
        sidecarNum = COUNT_OF(dxcSidecars);
    } else if (compilerType == COMPILER_SLANG) {
        sidecars = slangSidecars;
        sidecarNum = COUNT_OF(slangSidecars);
    } else if (compilerType == COMPILER_METAL_SHADER_CONVERTER) {
        sidecars = metalShaderConverterSidecars;
        sidecarNum = COUNT_OF(metalShaderConverterSidecars);
    }

    fs::path sidecarDirs[] = {
        path.parent_path(),
        path.parent_path().parent_path() / "lib",
        path.parent_path().parent_path() / "lib64",
    };

    for (const fs::path& sidecarDir : sidecarDirs) {
        for (size_t i = 0; i < sidecarNum; i++) {
            fs::path sidecarPath = sidecarDir / sidecars[i];
            if (!fs::exists(sidecarPath))
                continue;

            AppendBuildSignatureValue(signature, "compilerSidecar", PathToString(sidecarPath));
            AppendBuildSignatureValue(signature, "compilerSidecarSize", to_string(fs::file_size(sidecarPath)));
            AppendBuildSignatureValue(signature, "compilerSidecarTime", FileTimeToString(fs::last_write_time(sidecarPath)));
        }
    }
}

static string GetCompilerBuildSignature(const fs::path& compilerPath, CompilerType compilerType) {
    ostringstream signature;
    AppendBuildSignatureValue(signature, "version", "1");
    AppendCompilerBuildSignature(signature, compilerPath, compilerType);

    return signature.str();
}

static uint64_t GetFileContentsHash(const fs::path& path) {
    ifstream stream(path, ios::binary);
    ostringstream contents;
    contents << stream.rdbuf();

    return HashString(contents.str());
}

// Input files of "metal-shaderconverter" options: "--option=<file>" or "--option <file>", with one or two dashes
static vector<fs::path> GetMetalShaderConverterInputFiles(const vector<string>& metalShaderConverterOptions) {
    static const char* fileOptions[] = {"root-signature", "local-root-signature", "vertex-input-layout-file"};

    vector<string> tokens;
    for (const string& options : metalShaderConverterOptions) {
        string token;
        bool isToken = false;
        char quote = 0;
        for (char ch : options) {
            if (quote) {
                if (ch == quote)
                    quote = 0;
                else
                    token += ch;
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
                isToken = true;
            } else if (IsSpace(ch)) {
                if (isToken)
                    tokens.push_back(token);

                token.clear();
                isToken = false;
            } else {
                token += ch;
                isToken = true;
            }
        }

        if (isToken)
            tokens.push_back(token);
    }

    vector<fs::path> files;
    for (size_t i = 0; i < tokens.size(); i++) {
        const string& token = tokens[i];
        size_t nameBegin = token.find_first_not_of('-');
        if (nameBegin == 0 || nameBegin > 2 || nameBegin == string::npos)
            continue;

        size_t equal = token.find('=');
        string name = token.substr(nameBegin, equal == string::npos ? string::npos : equal - nameBegin);
        for (const char* fileOption : fileOptions) {
            if (name != fileOption)
                continue;

            if (equal != string::npos)
                files.push_back(token.substr(equal + 1));
            else if (i + 1 < tokens.size())
                files.push_back(tokens[++i]);
        }
    }

    return files;
}

static string GetBuildSignature() {
    fs::path outputPath = fs::absolute(g_Options.outputDir).lexically_normal();
    ostringstream signature;

    AppendBuildSignatureValue(signature, "version", "2");
    AppendBuildSignatureValue(signature, "config", PathToString(g_Options.configFile));
    AppendBuildSignatureValue(signature, "source", PathToString(g_Options.sourceDir));
    AppendBuildSignatureValue(signature, "output", PathToString(outputPath));
    AppendBuildSignatureValue(signature, "platform", g_Options.platformName);
    AppendBuildSignatureValue(signature, "outputExt", g_OutputExt);
    AppendCompilerBuildSignature(signature, g_Options.compiler, g_Options.compilerType);
    AppendBuildSignatureValue(signature, "shaderModel", g_Options.shaderModel);
    AppendBuildSignatureValue(signature, "optimization", to_string(g_Options.optimizationLevel));
    AppendBuildSignatureValue(signature, "slang", to_string(g_Options.slang));
    AppendBuildSignatureValue(signature, "slangHlsl", to_string(g_Options.slangHlsl));
    AppendBuildSignatureValue(signature, "warningsAreErrors", to_string(g_Options.warningsAreErrors));
    AppendBuildSignatureValue(signature, "allResourcesBound", to_string(g_Options.allResourcesBound));
    AppendBuildSignatureValue(signature, "pdb", to_string(g_Options.pdb));
    AppendBuildSignatureValue(signature, "embedPdb", to_string(g_Options.embedPdb));
    AppendBuildSignatureValue(signature, "stripReflection", to_string(g_Options.stripReflection));
    AppendBuildSignatureValue(signature, "matrixRowMajor", to_string(g_Options.matrixRowMajor));
    AppendBuildSignatureValue(signature, "hlsl2021", to_string(g_Options.hlsl2021));
    AppendBuildSignaturePaths(signature, "include", g_Options.includeDirs);
    AppendBuildSignaturePaths(signature, "relaxedInclude", g_Options.relaxedIncludes);
    AppendBuildSignatureValues(signature, "define", g_Options.defines);
    AppendBuildSignatureValues(signature, "compilerOptions", g_Options.compilerOptions);

    if (g_Options.platform == SPIRV) {
        AppendBuildSignatureValue(signature, "vulkanVersion", g_Options.vulkanVersion);
        AppendBuildSignatureValue(signature, "vulkanMemoryLayout", g_Options.vulkanMemoryLayout ? g_Options.vulkanMemoryLayout : "");
        AppendBuildSignatureValue(signature, "sRegShift", to_string(g_Options.sRegShift));
        AppendBuildSignatureValue(signature, "tRegShift", to_string(g_Options.tRegShift));
        AppendBuildSignatureValue(signature, "bRegShift", to_string(g_Options.bRegShift));
        AppendBuildSignatureValue(signature, "uRegShift", to_string(g_Options.uRegShift));
        AppendBuildSignatureValue(signature, "noRegShifts", to_string(g_Options.noRegShifts));
        AppendBuildSignatureValues(signature, "spirvExtension", g_Options.spirvExtensions);
    }

    if (g_Options.platform == METAL) {
        AppendBuildSignatureValue(signature, "metalSdk", g_Options.metalSdk);
        AppendBuildSignatureValue(signature, "metalStd", g_Options.metalStd);
        AppendBuildSignatureValue(signature, "metalMinOS", g_Options.metalMinOS);

        if (g_Options.metalFromDxil) {
            AppendBuildSignatureValues(signature, "metalShaderConverterOptions", g_Options.metalShaderConverterOptions);
            for (const fs::path& file : GetMetalShaderConverterInputFiles(g_Options.metalShaderConverterOptions))
                AppendBuildSignatureValue(signature, "metalShaderConverterInput", PathToString(file) + ":" + to_string(GetFileContentsHash(file)));

            AppendCompilerBuildSignature(signature, g_Options.metalShaderConverter, COMPILER_METAL_SHADER_CONVERTER);
        } else
            AppendCompilerBuildSignature(signature, g_Options.metalCompiler, COMPILER_METAL);
    }

    return signature.str();
}

// Multiple configs may share an output directory, so each one needs an independent signature.
static string GetBuildSignatureStem() {
    string configPath = PathToString(fs::absolute(g_Options.configFile));
#ifdef _WIN32
    transform(configPath.begin(), configPath.end(), configPath.begin(), [](char ch) { return (char)tolower((unsigned char)ch); });
#endif

    char configHash[17];
    snprintf(configHash, sizeof(configHash), "%016llX", (unsigned long long)HashString(configPath));

    return ".ShaderMake." + string(g_Options.platformName) + (g_Options.metalFromDxil ? "_DXIL." : ".") + configHash;
}

static fs::path GetBuildSignaturePath() {
    return fs::path(g_Options.outputDir) / (GetBuildSignatureStem() + ".signature");
}

static fs::path GetCompilerAliasBuildSignaturePath(const string& name) {
    return fs::path(g_Options.outputDir) / (GetBuildSignatureStem() + "." + name + ".compiler.signature");
}

static bool IsBuildSignatureCurrent(const fs::path& path, const string& signature) {
    ifstream stream(path, ios::binary);
    if (!stream)
        return false;

    ostringstream previousSignature;
    previousSignature << stream.rdbuf();

    return previousSignature.str() == signature;
}

static void Printf(const char* format, ...);

static bool InvalidateBuildSignature(const fs::path& path) {
    error_code errorCode;
    bool isRemoved = fs::remove(path, errorCode);
    if (!isRemoved && errorCode) {
        Printf(RED "ERROR: Can't invalidate build signature '%s'!\n", PathToString(path).c_str());
        return false;
    }

    return true;
}

static bool WriteBuildSignature(const fs::path& path, const string& signature) {
    fs::create_directories(path.parent_path());

    ofstream stream(path, ios::binary | ios::trunc);
    if (!stream) {
        Printf(RED "ERROR: Can't open build signature '%s' for writing!\n", PathToString(path).c_str());
        return false;
    }

    stream.write(signature.data(), signature.size());
    if (!stream) {
        Printf(RED "ERROR: Can't write build signature '%s'!\n", PathToString(path).c_str());
        return false;
    }

    return true;
}

static inline bool HasRepeatingSpace(char a, char b) {
    return (a == b) && a == ' ';
}

static string EscapePath(const string& s) {
    if (s.find(' ') != string::npos)
        return "\"" + s + "\"";

    return s;
}

static void TrimConfigLine(string& s) {
    // Remove leading whitespace
    s.erase(s.begin(), find_if(s.begin(), s.end(), [](char ch) { return !IsSpace(ch); }));

    // Remove trailing whitespace
    s.erase(find_if(s.rbegin(), s.rend(), [](char ch) { return !IsSpace(ch); }).base(), s.end());

    // Tabs to spaces
    replace(s.begin(), s.end(), '\t', ' ');

    // Remove double spaces
    string::iterator newEnd = unique(s.begin(), s.end(), HasRepeatingSpace);
    s.erase(newEnd, s.end());
}

static void TokenizeConfigLine(char* in, vector<const char*>& tokens) {
    char* out = in;
    char* token = out;

    // Some magic to correctly tokenize spaces in ""
    bool isString = false;
    while (*in) {
        if (*in == '"')
            isString = !isString;
        else if (*in == ' ' && !isString) {
            *in = '\0';
            if (*token)
                tokens.push_back(token);
            token = out + 1;
        }

        if (*in != '"')
            *out++ = *in;

        in++;
    }
    *out = '\0';

    if (*token)
        tokens.push_back(token);
}

static uint32_t GetFileLength(FILE* stream) {
    /*
    TODO: can be done more efficiently
    Win:
        #include <io.h>

        _filelength( _fileno(f) );
    Linux:
        #include <sys/types.h> //?
        #include <sys/stat.h>

        struct stat buf;
        fstat(fd, &buf);
        off_t size = buf.st_size;
    */

    const uint32_t pos = ftell(stream);
    fseek(stream, 0, SEEK_END);
    const uint32_t len = ftell(stream);
    fseek(stream, pos, SEEK_SET);

    return len;
}

static void Printf(const char* format, ...) {
    va_list argptr;
    va_start(argptr, format);

    // Remove embedded colors if colorization is off
    char fixedFormat[BUF_SIZE]; // TODO: let's assume that we always fit
    if (!g_Options.colorize) {
        const char* in = format;
        char* out = fixedFormat;

        while (*in) {
            if (*in == '\x1b')
                while (*in++ != 'm')
                    ;

            *out++ = *in++;
        }
        *out = '\0';

        format = fixedFormat;
    }

    // Print
    vprintf(format, argptr);
    va_end(argptr);

    // Restore default color if colorization is on
    if (g_Options.colorize)
        printf(WHITE);

    // IMPORTANT: needed only if being run in CMake environment
    fflush(stdout);
}

static const char* GetPlatformExt() {
    return g_Options.metalFromDxil ? g_MetalConverterBundleExt : g_PlatformExts[g_Options.platform];
}

static string GetShaderName(const fs::path& path) {
    string name = path.filename().string();
    replace(name.begin(), name.end(), '.', '_');
    name += "_" + string(GetPlatformExt() + 1);

    return "g_" + name;
}

// A class that is used to write a code blob as binary or C-string
class DataOutputContext {
public:
    FILE* stream = nullptr;

    DataOutputContext(const char* file, bool textMode) {
        stream = fopen(file, textMode ? "w" : "wb");
        if (!stream)
            Printf(RED "ERROR: Can't open file '%s' for writing!\n", file);
    }

    ~DataOutputContext() {
        if (stream) {
            fclose(stream);
            stream = nullptr;
        }
    }

    bool WriteDataAsText(const void* data, size_t size) {
        for (size_t i = 0; i < size; i++) {
            uint8_t value = ((const uint8_t*)data)[i];

            if (m_lineLength > 128) {
                fprintf(stream, "\n    ");
                m_lineLength = 0;
            }

            fprintf(stream, "%u,", value);

            if (value < 10)
                m_lineLength += 2;
            else if (value < 100)
                m_lineLength += 3;
            else
                m_lineLength += 4;
        }

        return true;
    }

    void WriteTextPreamble(const char* shaderName, const std::string& combinedDefines) {
        fprintf(stream, "// {%s}\n", combinedDefines.c_str());
        fprintf(stream, "const uint8_t %s[] = {", shaderName);
    }

    void WriteTextEpilog() {
        fprintf(stream, "\n};\n");
    }

    bool WriteDataAsBinary(const void* data, size_t size) {
        if (size == 0)
            return true;

        return fwrite(data, size, 1, stream) == 1;
    }

    // For use as a callback in "WriteFileHeader" and "WritePermutation" functions
    static bool WriteDataAsTextCallback(const void* data, size_t size, void* context) {
        return ((DataOutputContext*)context)->WriteDataAsText(data, size);
    }

    static bool WriteDataAsBinaryCallback(const void* data, size_t size, void* context) {
        return ((DataOutputContext*)context)->WriteDataAsBinary(data, size);
    }

private:
    uint32_t m_lineLength = 129;
};

static void UpdateProgress(TaskData& taskData, bool isSucceeded, bool willRetry, const char* message) {
    // IMPORTANT: do not split into several "Printf" calls because multi-threading access to the console can mess up the order
    if (isSucceeded) {
        float progress = 100.0f * float(++g_ProcessedTaskCount) / float(g_OriginalTaskCount);

        if (message && *message != '\0') {
            Printf(YELLOW "(%5.1f%%) %s %s %s {%s} {%s}\n%s",
                progress, g_Options.projectName, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str(),
                message);
        } else {
            if (g_Options.compactProgress) {
                uint32_t progressSnapped = (uint32_t(progress + 0.5f) / 10) * 10;
                lock_guard<mutex> guard(g_ProgressMutex);
                if (progressSnapped > g_PrevProgress) {
                    Printf(GREEN "(%3u%%)" GRAY " %s %s\n", progressSnapped, g_Options.projectName, g_Options.platformName);
                    g_PrevProgress = progressSnapped;
                }
            } else {
                Printf(GREEN "(%5.1f%%)" GRAY " %s %s" WHITE " %s" GRAY " {%s}" WHITE " {%s}\n",
                    progress, g_Options.projectName, g_Options.platformName,
                    taskData.source.c_str(),
                    taskData.entryPoint.c_str(),
                    taskData.combinedDefines.c_str());
            }
        }
    } else {
        // If retrying, requeue the task and try again without counting failure or terminating.
        if (willRetry) {
            Printf(YELLOW "( RETRY-QUEUED ) %s %s %s {%s} {%s}\n",
                g_Options.projectName, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str());

            lock_guard<mutex> guard(g_TaskMutex);
            g_TaskData.push_back(std::move(taskData));
        } else {
            Printf(RED "( FAIL ) %s %s %s {%s} {%s}\n%s",
                g_Options.projectName, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str(),
                message ? message : "<no message text>!\n");

            if (!g_Options.continueOnError)
                g_Terminate = true;

            ++g_FailedTaskCount;
        }
    }
}

//=====================================================================================================================
// TIMER
//=====================================================================================================================

double g_TicksToMilliseconds;

static double Timer_ConvertTicksToMilliseconds(uint64_t ticks) {
    return (double)ticks * g_TicksToMilliseconds;
}

static void Timer_Init() {
#ifdef _WIN32
    uint64_t ticksPerSecond = 1;
    QueryPerformanceFrequency((LARGE_INTEGER*)&ticksPerSecond);

    g_TicksToMilliseconds = 1000.0 / ticksPerSecond;
#else
    g_TicksToMilliseconds = 1.0 / 1000000.0;
#endif
}

static uint64_t Timer_GetTicks() {
#ifdef _WIN32
    uint64_t ticks;
    QueryPerformanceCounter((LARGE_INTEGER*)&ticks);

    return ticks;
#else
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);

    return uint64_t(spec.tv_sec) * 1000000000ull + spec.tv_nsec;
#endif
}

//=====================================================================================================================
// OPTIONS
//=====================================================================================================================

// Shader model in major_minor form for DXC/Slang profiles (e.g. 6_5, 6_10); each part is 1-4 decimal digits.
static bool ParseShaderModelVersion(const char* sm, uint32_t& outMajor, uint32_t& outMinor) {
    if (!sm || !sm[0])
        return false;

    const char* underscore = strchr(sm, '_');
    if (!underscore || underscore == sm || !underscore[1])
        return false;
    if (strchr(underscore + 1, '_'))
        return false;

    size_t majorLen = size_t(underscore - sm);
    size_t minorLen = strlen(underscore + 1);
    if (majorLen > 4 || minorLen > 4)
        return false;

    uint32_t major = 0;
    for (const char* p = sm; p < underscore; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        major = major * 10 + uint32_t(*p - '0');
    }

    uint32_t minor = 0;
    for (const char* p = underscore + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        minor = minor * 10 + uint32_t(*p - '0');
    }

    outMajor = major;
    outMinor = minor;
    return true;
}

static uint32_t GetSlangProfileMinSpirvVersion(const char* shaderModel) {
    uint32_t major = 0;
    uint32_t minor = 0;
    if (!ParseShaderModelVersion(shaderModel, major, minor) || major < 6)
        return 100;
    if (major == 6 && minor <= 2)
        return 103;
    if (major == 6 && minor <= 7)
        return 104;
    if (major == 6)
        return 105;

    return 106;
}

static uint32_t GetVulkanMaxSpirvVersion(const char* vulkanVersion) {
    if (strcmp(vulkanVersion, "1.0") == 0)
        return 100;
    if (strcmp(vulkanVersion, "1.1") == 0)
        return 103;
    if (strcmp(vulkanVersion, "1.1spirv1.4") == 0)
        return 104;
    if (strcmp(vulkanVersion, "1.2") == 0)
        return 105;

    return 106;
}

static const char* GetMetalMinOSOption(const char* metalSdk) {
    if (strcmp(metalSdk, "macosx") == 0)
        return "-mmacosx-version-min=";
    if (strcmp(metalSdk, "iphoneos") == 0)
        return "-mios-version-min=";
    if (strcmp(metalSdk, "iphonesimulator") == 0)
        return "-mios-simulator-version-min=";

    return nullptr;
}

#ifndef _WIN32
static bool FindMetalCompiler(const char* xcrun, const char* metalSdk, fs::path& outPath) {
    string cmd = EscapePath(xcrun) + " -sdk " + metalSdk + " -find metal 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return false;

    char buf[BUF_SIZE] = {};
    bool isRead = fgets(buf, sizeof(buf), pipe) != nullptr;
    if (pclose(pipe) != 0 || !isRead)
        return false;

    string path = buf;
    path.erase(find_if(path.rbegin(), path.rend(), [](char ch) { return !IsSpace(ch); }).base(), path.end());
    outPath = path;

    return !path.empty() && fs::exists(outPath);
}
#endif

static const char* GetMetalDeploymentOS(const char* metalSdk) {
    if (strcmp(metalSdk, "macosx") == 0)
        return "macOS";
    if (strcmp(metalSdk, "iphoneos") == 0)
        return "iOS";
    if (strcmp(metalSdk, "iphonesimulator") == 0)
        return "iOSSimulator";

    return nullptr;
}

// Windows: "%PROGRAMFILES%\<installDir>\bin" (current layout), "%PROGRAMFILES%\<installDir>\<targetDir>\bin" (per-target layout,
// "macos" or "ios"), then PATH. Others: PATH, then "/usr/local/bin"
static bool FindMetalTool(const char* name, const char* installDir, const char* targetDir, fs::path& outPath) {
    const char* paths = getenv("PATH");
#ifdef _WIN32
    const char* programFiles = getenv("PROGRAMFILES");
    string pathList;
    if (programFiles) {
        string root = string(programFiles) + "\\" + installDir + "\\";
        pathList += root + "bin;";
        if (targetDir)
            pathList += root + targetDir + "\\bin;";
    }
    pathList += paths ? paths : "";

    const char separator = ';';
    string fileName = string(name) + ".exe";
#else
    string pathList = paths ? paths : "";
    pathList += ":/usr/local/bin";

    const char separator = ':';
    string fileName = name;
    UNUSED(installDir);
    UNUSED(targetDir);
#endif

    size_t begin = 0;
    while (begin <= pathList.size()) {
        size_t end = pathList.find(separator, begin);
        if (end == string::npos)
            end = pathList.size();

        fs::path path = fs::path(pathList.substr(begin, end - begin)) / fileName;
        if (end != begin && fs::exists(path)) {
            outPath = path;
            return true;
        }

        begin = end + 1;
    }

    return false;
}

static int32_t AddInclude(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->includeDirs.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddGlobalDefine(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->defines.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddRelaxedInclude(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->relaxedIncludes.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddSpirvExtension(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->spirvExtensions.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddCompilerOptions(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->compilerOptions.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddMetalShaderConverterOptions(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->metalShaderConverterOptions.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddCompilerAlias(struct argparse* self, const struct argparse_option* option) {
    ((Options*)(option->data))->compilerAliasArgs.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

bool Options::Parse(int32_t argc, const char** argv) {
    const char* config = nullptr;
    const char* unused = nullptr; // storage for callbacks
    const char* srcDir = "";
    const char* metalShaderConverterPath = nullptr;
    bool ignoreConfigDir = false;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Required options:"),
        OPT_STRING('p', "platform", &platformName, "DXBC, DXIL, SPIRV or METAL", nullptr, 0, 0),
        OPT_STRING('c', "config", &config, "Configuration file with the list of shaders to compile", nullptr, 0, 0),
        OPT_STRING('o', "out", &outputDir, "Output directory", nullptr, 0, 0),
        OPT_BOOLEAN('b', "binary", &binary, "Output binary files", nullptr, 0, 0),
        OPT_BOOLEAN('h', "header", &header, "Output header files", nullptr, 0, 0),
        OPT_BOOLEAN('B', "binaryBlob", &binaryBlob, "Output binary blob files", nullptr, 0, 0),
        OPT_BOOLEAN('H', "headerBlob", &headerBlob, "Output header blob files", nullptr, 0, 0),
        OPT_STRING(0, "compiler", &compiler, "Path to an FXC/DXC/Slang compiler executable (METAL: xcrun on macOS, default = /usr/bin/xcrun, metal.exe on Windows, default = found in Program Files or PATH)", nullptr, 0, 0),
        OPT_GROUP("Compiler settings:"),
        OPT_STRING(0, "compilerAlias", &unused, "Alternate compiler for config-local selection in NAME=path form", AddCompilerAlias, (intptr_t)this, 0),
        OPT_STRING('m', "shaderModel", &shaderModel, "Shader model for DXIL/SPIRV (always SM 5.0 for DXBC): major_minor (e.g. 6_5, 6_10)", nullptr, 0, 0),
        OPT_INTEGER('O', "optimization", &optimizationLevel, "Optimization level 0-3 (default = 3, disabled = 0)", nullptr, 0, 0),
        OPT_STRING('X', "compilerOptions", &unused, "Custom command line options for the compiler, separated by spaces", AddCompilerOptions, (intptr_t)this, 0),
        OPT_BOOLEAN(0, "WX", &warningsAreErrors, "Treat warnings as errors", nullptr, 0, 0),
        OPT_BOOLEAN(0, "allResourcesBound", &allResourcesBound, "Maps to '-all_resources_bound' DXC/FXC option: all resources bound", nullptr, 0, 0),
        OPT_BOOLEAN(0, "PDB", &pdb, "Output PDB files in 'out/PDB/' folder when supported", nullptr, 0, 0),
        OPT_BOOLEAN(0, "embedPDB", &embedPdb, "Embed PDB with the shader binary", nullptr, 0, 0),
        OPT_BOOLEAN(0, "stripReflection", &stripReflection, "Maps to '-Qstrip_reflect' DXC/FXC option: strip reflection information from a shader binary", nullptr, 0, 0),
        OPT_BOOLEAN(0, "matrixRowMajor", &matrixRowMajor, "Maps to '-Zpr' DXC/FXC option: pack matrices in row-major order", nullptr, 0, 0),
        OPT_BOOLEAN(0, "hlsl2021", &hlsl2021, "Maps to '-HV 2021' DXC option: enable HLSL 2021 standard", nullptr, 0, 0),
        OPT_BOOLEAN(0, "slang", &slang, "Compiler is Slang", nullptr, 0, 0),
        OPT_BOOLEAN(0, "slangHLSL", &slangHlsl, "Use HLSL compatibility mode when compiler is Slang", nullptr, 0, 0),
        OPT_GROUP("Defines & include directories:"),
        OPT_STRING('I', "include", &unused, "Include directory(s)", AddInclude, (intptr_t)this, 0),
        OPT_STRING('D', "define", &unused, "Macro definition(s) in forms 'M=value' or 'M'", AddGlobalDefine, (intptr_t)this, 0),
        OPT_GROUP("Other options:"),
        OPT_BOOLEAN('f', "force", &force, "Treat all source files as modified", nullptr, 0, 0),
        OPT_STRING(0, "project", &projectName, "Project name to be seen in informational output", nullptr, 0, 0),
        OPT_STRING(0, "sourceDir", &srcDir, "Source code directory", nullptr, 0, 0),
        OPT_STRING(0, "relaxedInclude", &unused, "Include file(s) not invoking re-compilation", AddRelaxedInclude, (intptr_t)this, 0),
        OPT_STRING(0, "outputExt", &outputExt, "Extension for output files, default is one of .dxbc, .dxil, .spirv, .metallib", nullptr, 0, 0),
        OPT_BOOLEAN(0, "serial", &serial, "Disable multi-threading", nullptr, 0, 0),
        OPT_INTEGER('j', "jobs", &jobs, "Maximum number of parallel compilation tasks (default = number of logical processors)", nullptr, 0, 0),
        OPT_BOOLEAN(0, "flatten", &flatten, "Flatten source directory structure in the output directory", nullptr, 0, 0),
        OPT_BOOLEAN(0, "continue", &continueOnError, "Continue compilation if an error occurred", nullptr, 0, 0),
        OPT_BOOLEAN(0, "colorize", &colorize, "Colorize console output", nullptr, 0, 0),
        OPT_BOOLEAN(0, "verbose", &verbose, "Print commands before they are executed", nullptr, 0, 0),
        OPT_INTEGER(0, "retryCount", &retryCount, "Retry count for compilation task sub-process failures", nullptr, 0, 0),
        OPT_BOOLEAN(0, "ignoreConfigDir", &ignoreConfigDir, "Use 'current dir' instead of 'config dir' as parent path for relative dirs", nullptr, 0, 0),
        OPT_BOOLEAN(0, "compactProgress", &compactProgress, "Compact compilation progress reporting", nullptr, 0, 0),
        OPT_GROUP("SPIRV options:"),
        OPT_STRING(0, "vulkanMemoryLayout", &vulkanMemoryLayout, "Vulkan memory layout: dx, gl or scalar", nullptr, 0, 0),
        OPT_STRING(0, "vulkanVersion", &vulkanVersion, "Vulkan environment version (default = 1.3)", nullptr, 0, 0),
        OPT_STRING(0, "spirvExt", &unused, "Add a permitted SPIR-V extension", AddSpirvExtension, (intptr_t)this, 0),
        OPT_INTEGER(0, "sRegShift", &sRegShift, "SPIRV: register shift for sampler (s#) resources", nullptr, 0, 0),
        OPT_INTEGER(0, "tRegShift", &tRegShift, "SPIRV: register shift for texture (t#) resources", nullptr, 0, 0),
        OPT_INTEGER(0, "bRegShift", &bRegShift, "SPIRV: register shift for constant (b#) resources", nullptr, 0, 0),
        OPT_INTEGER(0, "uRegShift", &uRegShift, "SPIRV: register shift for UAV (u#) resources", nullptr, 0, 0),
        OPT_BOOLEAN(0, "noRegShifts", &noRegShifts, "Don't specify any register shifts for the compiler", nullptr, 0, 0),
        OPT_GROUP("METAL options:"),
        OPT_STRING(0, "metalSdk", &metalSdk, "Target SDK: macosx, iphoneos or iphonesimulator (default = macosx)", nullptr, 0, 0),
        OPT_STRING(0, "metalStd", &metalStd, "Metal language standard (default = metal4.0)", nullptr, 0, 0),
        OPT_STRING(0, "metalMinOS", &metalMinOS, "Minimum OS version for the selected SDK (default = 26.0)", nullptr, 0, 0),
        OPT_BOOLEAN(0, "metalFromDXIL", &metalFromDxil, "Compile HLSL to DXIL, convert it with 'metal-shaderconverter' and output Metal converter bundles", nullptr, 0, 0),
        OPT_STRING(0, "metalShaderConverter", &metalShaderConverterPath, "Path to 'metal-shaderconverter' (default = found in PATH or /usr/local/bin, on Windows in Program Files or PATH)", nullptr, 0, 0),
        OPT_STRING(0, "metalShaderConverterOptions", &unused, "Custom command line options for 'metal-shaderconverter', separated by spaces", AddMetalShaderConverterOptions, (intptr_t)this, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "ShaderMake.exe -p {DXBC|DXIL|SPIRV|METAL} [-b] [-h] [-B] [-H] -c \"path/to/config\"\n"
        "\t-o \"path/to/output\" --compiler \"path/to/compiler\" [--compilerAlias \"NAME=path\"] [other options]\n"
        "\t-D DEF1 -D DEF2=1 ... -I \"path1\" -I \"path2\" ...",
        nullptr};

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nMulti-threaded shader compiling & processing tool");
    argparse_parse(&argparse, argc, argv);

    if (!config) {
        Printf(RED "ERROR: Config file not specified!\n");
        return false;
    }

    if (!fs::exists(config)) {
        Printf(RED "ERROR: Config file '%s' does not exist!\n", config);
        return false;
    }

    if (!outputDir) {
        Printf(RED "ERROR: Output directory not specified!\n");
        return false;
    }

    if (!binary && !header && !binaryBlob && !headerBlob) {
        Printf(RED "ERROR: One of 'binary', 'header', 'binaryBlob' or 'headerBlob' must be set!\n");
        return false;
    }
    if (!platformName) {
        Printf(RED "ERROR: Platform not specified!\n");
        return false;
    }

    // Platform
    uint32_t i = 0;
    for (; i < PLATFORMS_NUM; i++) {
        if (!strcmp(platformName, g_PlatformNames[i])) {
            platform = (Platform)i;
            break;
        }
    }
    if (i == PLATFORMS_NUM) {
        Printf(RED "ERROR: Unrecognized platform '%s'!\n", platformName);
        return false;
    }

    if (platform == METAL) {
#ifdef __APPLE__
        if (!compiler && !metalFromDxil)
            compiler = "/usr/bin/xcrun";
#elif defined(_WIN32)
        static string metalCompilerPath;
        if (!compiler && !metalFromDxil) {
            const char* targetDir = strcmp(metalSdk, "macosx") == 0 ? "macos" : "ios";
            if (!FindMetalTool("metal", "Metal Developer Tools", targetDir, metalCompiler)) {
                Printf(RED "ERROR: Can't find 'metal.exe' (install Metal Developer Tools for Windows or use --compiler)!\n");
                return false;
            }

            metalCompilerPath = PathToString(metalCompiler);
            compiler = metalCompilerPath.c_str();
        }
#else
        Printf(RED "ERROR: METAL platform is only supported on macOS and Windows!\n");
        return false;
#endif
    }

    if (!compiler) {
        Printf(RED "ERROR: Compiler not specified!\n");
        return false;
    }

    if (!fs::exists(compiler)) {
        Printf(RED "ERROR: Compiler '%s' does not exist!\n", compiler);
        return false;
    }

    uint32_t smMajor = 0, smMinor = 0;
    if (!ParseShaderModelVersion(shaderModel, smMajor, smMinor)) {
        Printf(RED "ERROR: Shader model ('%s') must be major_minor with decimal digits (e.g. '6_5', '6_10')!\n",
            shaderModel ? shaderModel : "");
        return false;
    }

    // Keep --slang as an explicit override for wrappers and non-standard executable names.
    slang = slang || IsSlangCompiler(compiler);
    compilerType = slang ? COMPILER_SLANG : (platform == DXBC ? COMPILER_FXC : COMPILER_DXC);

    if (platform == METAL) {
        if (slang) {
            Printf(RED "ERROR: Slang is not supported for METAL target!\n");
            return false;
        }

        if (!GetMetalMinOSOption(metalSdk)) {
            Printf(RED "ERROR: Unsupported value '%s' for --metalSdk! Only 'macosx', 'iphoneos' and 'iphonesimulator' are supported.\n", metalSdk);
            return false;
        }

        if (metalFromDxil) {
            if (metalShaderConverterPath)
                metalShaderConverter = metalShaderConverterPath;
            else if (!FindMetalTool("metal-shaderconverter", "Metal Shader Converter", nullptr, metalShaderConverter)) {
                Printf(RED "ERROR: Can't find 'metal-shaderconverter' (use --metalShaderConverter)!\n");
                return false;
            }

            if (!fs::exists(metalShaderConverter)) {
                Printf(RED "ERROR: Metal shader converter '%s' does not exist!\n", PathToString(metalShaderConverter).c_str());
                return false;
            }
        } else {
#ifdef _WIN32
            // No SDKs on Windows: "metal.exe" is the driver, "--metalSdk" only selects the deployment target
            metalCompiler = compiler;
#else
            if (!FindMetalCompiler(compiler, metalSdk, metalCompiler)) {
                Printf(RED "ERROR: Can't find Metal compiler for SDK '%s' (is the Metal toolchain installed?)!\n", metalSdk);
                return false;
            }
#endif

            compilerType = COMPILER_METAL;
        }
    } else if (metalFromDxil) {
        Printf(RED "ERROR: --metalFromDXIL is only supported for METAL target!\n");
        return false;
    }

    if (!metalShaderConverterOptions.empty() && !metalFromDxil) {
        Printf(RED "ERROR: --metalShaderConverterOptions requires --metalFromDXIL!\n");
        return false;
    }

    for (const string& compilerAliasArg : compilerAliasArgs) {
        size_t separatorPos = compilerAliasArg.find('=');
        string name = ToUpper(compilerAliasArg.substr(0, separatorPos));
        fs::path path = separatorPos == string::npos ? fs::path() : fs::path(compilerAliasArg.substr(separatorPos + 1));
        CompilerType type;

        if (separatorPos == string::npos || name.empty() || path.empty()) {
            Printf(RED "ERROR: Compiler alias '%s' must use NAME=path form!\n", compilerAliasArg.c_str());
            return false;
        }

        if (!GetCompilerType(name, type)) {
            Printf(RED "ERROR: Unsupported compiler alias '%s'! Only DXC and SLANG are supported.\n", name.c_str());
            return false;
        }

        if (!fs::exists(path)) {
            Printf(RED "ERROR: Compiler alias '%s' refers to missing file '%s'!\n", name.c_str(), PathToString(path).c_str());
            return false;
        }

        if (FindCompilerAlias(name.c_str())) {
            Printf(RED "ERROR: Compiler alias '%s' is specified more than once!\n", name.c_str());
            return false;
        }

        compilerAliases.push_back({name, path, type});
    }

    if (outputExt)
        g_OutputExt = outputExt;
    else
        g_OutputExt = GetPlatformExt();

    if (g_Options.vulkanMemoryLayout && platform != SPIRV) {
        Printf(RED "ERROR: --vulkanMemoryLayout is only supported for SPIRV target!\n");
        return false;
    }

    if (vulkanMemoryLayout && strcmp(vulkanMemoryLayout, "dx") != 0 && strcmp(vulkanMemoryLayout, "gl") != 0 && strcmp(vulkanMemoryLayout, "scalar") != 0) {
        Printf(RED "ERROR: Unsupported value '%s' for --vulkanMemoryLayout! Only 'dx', 'gl' and 'scalar' are supported.\n",
            vulkanMemoryLayout);
        return false;
    }

    if (slang && platform == SPIRV && !GetSlangSpirvCapability(vulkanVersion)) {
        Printf(RED
            "ERROR: Unsupported value '%s' for --vulkanVersion and Slang! Only '1.0', '1.1', "
            "'1.1spirv1.4', '1.2', '1.3' and '1.4' are supported.\n",
            vulkanVersion);
        return false;
    }

    if (g_Options.retryCount < 0) {
        Printf(RED "ERROR: --retryCount must be greater than or equal to 0.\n");
        return false;
    }

    if (jobs < 0) {
        Printf(RED "ERROR: --jobs must be greater than or equal to 0.\n");
        return false;
    }

    // Absolute path is needed for source files to get "clickable" messages
#ifdef _WIN32
    char cd[MAX_PATH];
    if (!GetCurrentDirectoryA(sizeof(cd), cd))
#else
    char cd[PATH_MAX];
    if (!getcwd(cd, sizeof(cd)))
#endif
    {
        Printf(RED "ERROR: Cannot get the working directory!\n");
        return false;
    }

    configFile = fs::path(cd) / fs::path(config);

    fs::path fsSrcDir = srcDir;
    if (fsSrcDir.is_relative()) {
        if (ignoreConfigDir)
            g_Options.sourceDir = fs::path(cd) / fsSrcDir;
        else
            g_Options.sourceDir = g_Options.configFile.parent_path() / fsSrcDir;
    } else
        g_Options.sourceDir = fsSrcDir;

    for (fs::path& path : includeDirs) {
        if (path.is_relative()) {
            if (ignoreConfigDir)
                path = fs::path(cd) / path;
            else
                path = configFile.parent_path() / path;
        }
    }

    return true;
}

static int32_t AddLocalDefine(struct argparse* self, const struct argparse_option* option) {
    ((ConfigLine*)(option->data))->defines.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddLocalCompilerOptions(struct argparse* self, const struct argparse_option* option) {
    ((ConfigLine*)(option->data))->compilerOptions.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddCompilerOptionsDXIL(struct argparse* self, const struct argparse_option* option) {
    ((ConfigLine*)(option->data))->compilerOptionsDXIL.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddCompilerOptionsSPIRV(struct argparse* self, const struct argparse_option* option) {
    ((ConfigLine*)(option->data))->compilerOptionsSPIRV.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

static int32_t AddLocalMetalShaderConverterOptions(struct argparse* self, const struct argparse_option* option) {
    ((ConfigLine*)(option->data))->metalShaderConverterOptions.push_back(*(const char**)option->value);
    UNUSED(self);
    return 0;
}

bool ConfigLine::Parse(int32_t argc, const char** argv) {
    source = argv[0];

    const char* unused = nullptr; // storage for the callback
    struct argparse_option options[] = {
        OPT_STRING('T', "profile", &profile, "Shader profile", nullptr, 0, 0),
        OPT_STRING('E', "entryPoint", &entryPoint, "(Optional) entry point", nullptr, 0, 0),
        OPT_STRING('D', "define", &unused, "(Optional) define(s) in forms 'M=value' or 'M'", AddLocalDefine, (intptr_t)this, 0),
        OPT_STRING('o', "output", &outputDir, "(Optional) output subdirectory", nullptr, 0, 0),
        OPT_INTEGER('O', "optimization", &optimizationLevel, "(Optional) optimization level", nullptr, 0, 0),
        OPT_STRING('s', "outputSuffix", &outputSuffix, "(Optional) suffix to insert before the first filename extension", nullptr, 0, 0),
        OPT_STRING('m', "shaderModel", &shaderModel, "(Optional) shader model for DXIL/SPIRV (always SM 5.0 for DXBC): major_minor (e.g. 6_5, 6_10)", nullptr, 0, 0),
        OPT_STRING(0, "compilerDXIL", &compilerDXIL, "(Optional) compiler alias used only for DXIL", nullptr, 0, 0),
        OPT_STRING(0, "compilerSPIRV", &compilerSPIRV, "(Optional) compiler alias used only for SPIRV", nullptr, 0, 0),
        OPT_STRING('X', "compilerOptions", &unused, "Custom command line options for the compiler, separated by spaces", AddLocalCompilerOptions, (intptr_t)this, 0),
        OPT_BOOLEAN(0, "noRegShifts", &noRegShifts, "Don't specify any register shifts for this shader", nullptr, 0, 0),

        OPT_STRING(0, "compilerOptionsDXIL", &unused, "Custom command line options for dxil, separated by spaces", AddCompilerOptionsDXIL, (intptr_t)this, 0),
        OPT_STRING(0, "compilerOptionsSPIRV", &unused, "Custom command line options for spirv, separated by spaces", AddCompilerOptionsSPIRV, (intptr_t)this, 0),
        OPT_STRING(0, "metalShaderConverterOptions", &unused, "Custom command line options for 'metal-shaderconverter', separated by spaces", AddLocalMetalShaderConverterOptions, (intptr_t)this, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "path/to/shader -T profile [-E entry] [-O{0|1|2|3}] [-o \"output/subdirectory\"] [-s \"suffix\"] [-m 6_5] [--compilerDXIL NAME] [--compilerSPIRV NAME] [-D DEF1={0,1}] [-D DEF2={0,1,2}] [-D DEF3] [-X \"options\"] [...]",
        nullptr};

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nConfiguration options for a shader");
    argparse_parse(&argparse, argc, argv);

    if (!shaderModel)
        shaderModel = g_Options.shaderModel;

    // A ".metallib" is a library with any number of functions
    if (!profile && g_Options.platform == METAL && !g_Options.metalFromDxil)
        profile = "lib";

    // If there are some non-option elements in the config line, they will remain in the argv array.
    if (argv[0]) {
        Printf(RED "ERROR: Unrecognized element in the config line: '%s'!\n", argv[0]);
        return false;
    }

    if (!profile) {
        Printf(RED "ERROR: Shader target not specified!\n");
        return false;
    }

    uint32_t smMajor = 0, smMinor = 0;
    if (!ParseShaderModelVersion(shaderModel, smMajor, smMinor)) {
        Printf(RED "ERROR: Shader model ('%s') must be major_minor with decimal digits (e.g. '6_5', '6_10')!\n", shaderModel);
        return false;
    }

    return true;
}

static bool ReadBinaryFile(const char* file, vector<uint8_t>& outData) {
    FILE* stream = fopen(file, "rb");
    if (!stream) {
        Printf(RED "ERROR: Can't open file '%s'!\n", file);
        return false;
    }

    uint32_t const binarySize = GetFileLength(stream);
    if (binarySize == 0) {
        Printf(RED "ERROR: Binary file '%s' is empty!\n", file);
        fclose(stream);
        return false;
    }

    // Warn if the file is suspiciously large
    if (binarySize > (64 << 20)) // > 64Mb
        Printf(YELLOW "WARNING: Binary file '%s' is too large!\n", file);

    // Allocate memory foe the whole file
    outData.resize(binarySize);

    // Read the file
    size_t bytesRead = fread(outData.data(), 1, binarySize, stream);
    bool success = (bytesRead == binarySize);

    fclose(stream);
    return success;
}

static bool TryReserveRetry() {
    int32_t retryCount = g_TaskRetryCount.load();
    while (retryCount > 0) {
        if (g_TaskRetryCount.compare_exchange_weak(retryCount, retryCount - 1))
            return true;
    }

    return false;
}

static bool WriteMetalConverterBundle(const string& metallibFile, const string& reflectionFile, const string& outputFile, ostringstream& msg) {
    vector<uint8_t> metallib;
    vector<uint8_t> reflection;
    if (!ReadBinaryFile(metallibFile.c_str(), metallib) || !ReadBinaryFile(reflectionFile.c_str(), reflection)) {
        msg << "ERROR: Can't read 'metal-shaderconverter' output!\n";
        return false;
    }

    // Layout: header, metallib (8-byte aligned), reflection JSON, zero terminator
    ShaderMake::MetalConverterBundleHeader header = {};
    header.magic = ShaderMake::MetalConverterBundleMagic;
    header.version = ShaderMake::MetalConverterBundleVersion;
    header.metallibOffset = (uint32_t)((sizeof(header) + 7) & ~size_t(7));
    header.metallibSize = (uint32_t)metallib.size();
    header.reflectionOffset = header.metallibOffset + header.metallibSize;
    header.reflectionSize = (uint32_t)reflection.size();

    vector<uint8_t> bundle(header.reflectionOffset + reflection.size() + 1);
    memcpy(bundle.data(), &header, sizeof(header));
    memcpy(bundle.data() + header.metallibOffset, metallib.data(), metallib.size());
    memcpy(bundle.data() + header.reflectionOffset, reflection.data(), reflection.size());

    DataOutputContext context(outputFile.c_str(), false);
    if (!context.stream || !context.WriteDataAsBinary(bundle.data(), bundle.size())) {
        msg << "ERROR: Can't write '" << outputFile << "'!\n";
        return false;
    }

    return true;
}

static void ExeCompile() {
    static const char* optimizationLevelRemap[] = {
        " -Od",
        " -O1",
        " -O2",
        " -O3",
    };

    while (!g_Terminate) {
        // Getting a task in the current thread
        TaskData taskData;
        {
            lock_guard<mutex> guard(g_TaskMutex);
            if (g_TaskData.empty())
                return;

            taskData = std::move(g_TaskData.back());
            g_TaskData.pop_back();
        }

        string outputFile = taskData.outputFileWithoutExt + g_OutputExt;

        // METAL from DXIL: intermediate files for the bundle, next to the output
        string compilerOutputFile = g_Options.metalFromDxil ? outputFile + ".dxil" : outputFile;
        string metallibFile = outputFile + ".metallib";
        string reflectionFile = outputFile + ".json";

        // Building command line
        ostringstream cmd;
        {
            cmd << EscapePath(taskData.compiler);

            if (taskData.compilerType == COMPILER_METAL) {
                // The "metal" driver compiles and links in one step, intermediate ".air" files are managed by the driver
#ifndef _WIN32
                cmd << " -sdk " << g_Options.metalSdk << " metal";
#endif

                // Output
                cmd << " -o " << EscapePath(outputFile);

                // Language standard and deployment target
                cmd << " -std=" << g_Options.metalStd;
                cmd << " " << GetMetalMinOSOption(g_Options.metalSdk) << g_Options.metalMinOS;

                // Defines
                for (const string& define : taskData.defines)
                    cmd << " -D" << define;

                for (const string& define : g_Options.defines)
                    cmd << " -D" << define;

                // Include directories
                for (const fs::path& dir : g_Options.includeDirs)
                    cmd << " -I " << EscapePath(dir.string());

                // Optimization level
                cmd << " -O" << taskData.optimizationLevel;

                // Warnings as errors
                if (g_Options.warningsAreErrors)
                    cmd << " -Werror";

                // Debug info with sources, embedded or in a separate ".metallibsym" file next to the output
                if (g_Options.embedPdb)
                    cmd << " -gline-tables-only -frecord-sources";
                else if (g_Options.pdb)
                    cmd << " -gline-tables-only -frecord-sources=flat";

                // Custom options
                AppendCompilerOptions(cmd, g_Options.compilerOptions, taskData.compilerType);
                AppendCompilerOptions(cmd, taskData.compilerOptions, taskData.compilerType);
            } else if (taskData.compilerType == COMPILER_SLANG) {
                // Slang defaults to slang language mode unless -lang <other language> sets something else.
                // For HLSL compatibility mode:
                //    - use -lang hlsl to set language mode to HLSL
                //    - use -unscoped-enums so Slang doesn't require all enums to be scoped
                if (g_Options.slangHlsl) {
                    // Language mode: hlsl
                    cmd << " -lang hlsl";

                    // Treat enums as unscoped
                    cmd << " -unscoped-enum";
                }

                // Profile
                cmd << " -profile " << taskData.profile << "_" << taskData.shaderModel;

                // Target/platform
                cmd << " -target " << g_PlatformSlangTargets[g_Options.platform];
                if (g_Options.platform == SPIRV) {
                    const char* spirvCapability = GetSlangSpirvCapability(g_Options.vulkanVersion);
                    if (spirvCapability)
                        cmd << " -capability " << spirvCapability;

                    for (const string& extension : g_Options.spirvExtensions) {
                        if (extension != "KHR" && extension != "SPV_EXT_mesh_shader")
                            cmd << " -capability " << extension;
                    }
                }

                // Output
                cmd << " -o " << EscapePath(outputFile);

                // Entry point
                if (taskData.profile != "lib") {
                    // Don't specify entry if profile is lib_*, Slang will use the entry point currently
                    cmd << " -entry " << taskData.entryPoint;
                }

                // Defines
                for (const string& define : taskData.defines)
                    cmd << " -D" << define;

                for (const string& define : g_Options.defines)
                    cmd << " -D" << define;

                // Include directories
                for (const fs::path& dir : g_Options.includeDirs)
                    cmd << " -I " << EscapePath(dir.string());

                // Optimization level
                cmd << " -O" << taskData.optimizationLevel;

                // Warnings as errors
                if (g_Options.warningsAreErrors)
                    cmd << " -warnings-as-errors all";

                // Matrix layout
                if (g_Options.matrixRowMajor)
                    cmd << " -matrix-layout-row-major";
                else
                    cmd << " -matrix-layout-column-major";

                // Debug symbols
                if (g_Options.pdb || g_Options.embedPdb)
                    cmd << " -g";

                if (g_Options.platform == SPIRV) {
                    // Uses the entrypoint name from the source instead of 'main' in the SPIRV output
                    cmd << " -fvk-use-entrypoint-name";

                    if (g_Options.vulkanMemoryLayout) {
                        if (strcmp(g_Options.vulkanMemoryLayout, "scalar") == 0)
                            cmd << " -force-glsl-scalar-layout";
                        else if (strcmp(g_Options.vulkanMemoryLayout, "gl") == 0)
                            cmd << " -fvk-use-gl-layout";
                        else if (strcmp(g_Options.vulkanMemoryLayout, "dx") == 0)
                            cmd << " -fvk-use-dx-layout";
                    }

                    if (!g_Options.noRegShifts && !taskData.noRegShifts) {
                        for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++) {
                            cmd << " -fvk-s-shift " << g_Options.sRegShift << " " << space;
                            cmd << " -fvk-t-shift " << g_Options.tRegShift << " " << space;
                            cmd << " -fvk-b-shift " << g_Options.bRegShift << " " << space;
                            cmd << " -fvk-u-shift " << g_Options.uRegShift << " " << space;
                        }
                    }
                }

                // Custom options
                AppendCompilerOptions(cmd, g_Options.compilerOptions, taskData.compilerType);
                AppendCompilerOptions(cmd, taskData.compilerOptions, taskData.compilerType);

                // Platform-specific custom options
                if (g_Options.platform == DXIL)
                    AppendCompilerOptions(cmd, taskData.compilerOptionsDXIL, taskData.compilerType);
                else if (g_Options.platform == SPIRV)
                    AppendCompilerOptions(cmd, taskData.compilerOptionsSPIRV, taskData.compilerType);
            } else {
                cmd << " -nologo";

                // Output file
                cmd << " -Fo " << EscapePath(compilerOutputFile);

                // Profile
                string profile = taskData.profile + "_";
                if (g_Options.platform == DXBC)
                    profile += "5_0";
                else
                    profile += taskData.shaderModel;
                cmd << " -T " << profile;

                // Entry point
                cmd << " -E " << taskData.entryPoint;

                // Defines
                for (const string& define : taskData.defines)
                    cmd << " -D " << define;

                for (const string& define : g_Options.defines)
                    cmd << " -D " << define;

                // Include directories
                for (const fs::path& dir : g_Options.includeDirs)
                    cmd << " -I " << EscapePath(dir.string());

                // Args
                cmd << optimizationLevelRemap[taskData.optimizationLevel];

                uint32_t smMajor = 0, smMinor = 0;
                if (ParseShaderModelVersion(taskData.shaderModel.c_str(), smMajor, smMinor) && g_Options.platform != DXBC && (smMajor > 6 || (smMajor == 6 && smMinor >= 2)))
                    cmd << " -enable-16bit-types";

                if (g_Options.warningsAreErrors)
                    cmd << " -WX";

                if (g_Options.allResourcesBound)
                    cmd << " -all_resources_bound";

                if (g_Options.matrixRowMajor)
                    cmd << " -Zpr";

                if (g_Options.hlsl2021)
                    cmd << " -HV 2021";

                if (g_Options.pdb || g_Options.embedPdb)
                    cmd << " -Zi -Zsb"; // only binary affects hash

                if (g_Options.embedPdb)
                    cmd << " -Qembed_debug";

                if (g_Options.platform == SPIRV) {
                    cmd << " -spirv";

                    cmd << " -fspv-target-env=vulkan" << g_Options.vulkanVersion;

                    if (g_Options.vulkanMemoryLayout)
                        cmd << " -fvk-use-" << g_Options.vulkanMemoryLayout << "-layout";

                    for (const string& ext : g_Options.spirvExtensions)
                        cmd << " -fspv-extension=" << ext;

                    if (!g_Options.noRegShifts && !taskData.noRegShifts) {
                        for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++) {
                            cmd << " -fvk-s-shift " << g_Options.sRegShift << " " << space;
                            cmd << " -fvk-t-shift " << g_Options.tRegShift << " " << space;
                            cmd << " -fvk-b-shift " << g_Options.bRegShift << " " << space;
                            cmd << " -fvk-u-shift " << g_Options.uRegShift << " " << space;
                        }
                    }
                } else // Not supported by SPIRV gen
                {
                    if (g_Options.stripReflection)
                        cmd << " -Qstrip_reflect";

                    if (g_Options.pdb) {
                        fs::path pdbPath = fs::path(outputFile).parent_path() / PDB_DIR;
                        cmd << " -Fd " << EscapePath(pdbPath.string() + "/"); // only binary code affects hash
                    }
                }

                // Custom options
                AppendCompilerOptions(cmd, g_Options.compilerOptions, taskData.compilerType);
                AppendCompilerOptions(cmd, taskData.compilerOptions, taskData.compilerType);

                // Platform-specific custom options
                if (g_Options.platform == DXIL || g_Options.metalFromDxil)
                    AppendCompilerOptions(cmd, taskData.compilerOptionsDXIL, taskData.compilerType);
                else if (g_Options.platform == SPIRV)
                    AppendCompilerOptions(cmd, taskData.compilerOptionsSPIRV, taskData.compilerType);
            }

            // Source file
            fs::path sourceFile = g_Options.sourceDir / taskData.source;
            cmd << " " << EscapePath(sourceFile.string());

            // METAL from DXIL: conversion, custom options are passed as is
            if (g_Options.metalFromDxil) {
                cmd << " 2>&1 && " << EscapePath(PathToString(g_Options.metalShaderConverter));
                cmd << " " << EscapePath(compilerOutputFile);
                cmd << " --entry-point=" << taskData.entryPoint;
                cmd << " --deployment-os=" << GetMetalDeploymentOS(g_Options.metalSdk);
                cmd << " --minimum-os-build-version=" << g_Options.metalMinOS;

                // The converter expects "major.minor.patch"
                const char* metalMinOS = g_Options.metalMinOS;
                for (ptrdiff_t dots = count(metalMinOS, metalMinOS + strlen(metalMinOS), '.'); dots < 2; dots++)
                    cmd << ".0";

                cmd << " -o " << EscapePath(metallibFile);
                cmd << " " << EscapePath("--output-reflection-file=" + reflectionFile);

                for (const vector<string>* options : {&g_Options.metalShaderConverterOptions, &taskData.metalShaderConverterOptions}) {
                    for (const string& option : *options)
                        cmd << " " << option;
                }
            }
        }

        cmd << " 2>&1";

        // Debug output
        if (g_Options.verbose)
            Printf(WHITE "%s\n", cmd.str().c_str());

        // "cmd.exe /c" strips the first and the last quotes, keeping quoted paths (like "Program Files") intact
#ifdef _WIN32
        string cmdLine = "\"" + cmd.str() + "\"";
#else
        string cmdLine = cmd.str();
#endif

        // Compiling the shader
        ostringstream msg;
        FILE* pipe = popen(cmdLine.c_str(), "r");

        bool isSucceeded = false;
        bool willRetry = false;
        if (pipe) {
            char buf[BUF_SIZE];
            while (fgets(buf, sizeof(buf), pipe)) {
                // Ignore useless unmutable FXC messages: "compilation header/object save succeeded"
                if (g_Options.platform == DXBC && !strncmp(buf, "compilation ", 12))
                    continue;

                msg << buf;
            }

            int32_t result = pclose(pipe);
            // Check status, see https://pubs.opengroup.org/onlinepubs/009696699/functions/pclose.html
            bool childProcessError = (result == -1 && errno == ECHILD);
#ifdef _WIN32
            bool commandShellError = false;
#else
            bool commandShellError = result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 127;
#endif

            if (result == 0)
                isSucceeded = true;

            // Retry if count > 0 and failed to execute child sub-process or command shell (posix only)
            else if ((childProcessError || commandShellError) && TryReserveRetry())
                willRetry = true;
        } else
            willRetry = TryReserveRetry();

        // Bundle converter output
        if (g_Options.metalFromDxil) {
            if (isSucceeded && !WriteMetalConverterBundle(metallibFile, reflectionFile, outputFile, msg))
                isSucceeded = false;

            error_code errorCode;
            fs::remove(compilerOutputFile, errorCode);
            fs::remove(metallibFile, errorCode);
            fs::remove(reflectionFile, errorCode);
        }

        // Convert to headers if needed
        if (isSucceeded && (g_Options.header || (g_Options.headerBlob && taskData.combinedDefines.empty()))) {
            vector<uint8_t> buffer;
            if (ReadBinaryFile(outputFile.c_str(), buffer)) {
                string headerFile = taskData.outputFileWithoutExt + g_OutputExt + ".h";
                DataOutputContext context(headerFile.c_str(), true);
                if (context.stream) {
                    string shaderName = GetShaderName(taskData.outputFileWithoutExt);
                    context.WriteTextPreamble(shaderName.c_str(), taskData.combinedDefines);
                    context.WriteDataAsText(buffer.data(), buffer.size());
                    context.WriteTextEpilog();

                    // Try to delete the binary file if it's not requested.
                    // In the unlikely event it fails (one system sometimes holds the file handle too long),
                    // avoid a fatal exit because it's merely an intermediate file.
                    if (!g_Options.binary) {
                        try {
                            fs::remove(outputFile);
                        } catch (const std::exception& e) {
                            Printf(YELLOW "Could not delete temporary binary file '%s': %s\n", outputFile.c_str(), e.what());
                        }
                    }
                } else {
                    Printf(RED "ERROR: Failed to open file '%s' for writing!\n", headerFile.c_str());
                    isSucceeded = false;
                }
            } else
                isSucceeded = false;
        }

        // Update progress
        UpdateProgress(taskData, isSucceeded, willRetry, msg.str().c_str());
    }
}

//=====================================================================================================================
// MAIN
//=====================================================================================================================

static bool IsMetalToolchainHeader(const fs::path& includeName) {
    const string name = includeName.generic_string();

    return name.rfind("metal_", 0) == 0 || name.rfind("simd/", 0) == 0;
}

static bool GetHierarchicalUpdateTime(const fs::path& file, list<fs::path>& callStack, fs::file_time_type& outTime) {
    static const basic_regex<char> includePattern("\\s*#include\\s+([\"<])([^>\"]+)[>\"].*");

    auto found = g_HierarchicalUpdateTimes.find(file);
    if (found != g_HierarchicalUpdateTimes.end()) {
        outTime = found->second;

        return true;
    }

    ifstream stream(file);
    if (!stream.is_open()) {
        Printf(RED "ERROR: Can't open file '%s', included in:\n", PathToString(file).c_str());
        for (const fs::path& otherFile : callStack)
            Printf(RED "\t%s\n", PathToString(otherFile).c_str());

        return false;
    }

    callStack.push_front(file);

    fs::path path = file.parent_path();
    fs::file_time_type hierarchicalUpdateTime = fs::last_write_time(file);

    for (string line; getline(stream, line);) {
        match_results<const char*> matchResult;
        regex_match(line.c_str(), matchResult, includePattern);
        if (matchResult.empty())
            continue;

        fs::path includeName = string(matchResult[2]);
        if (find(g_Options.relaxedIncludes.begin(), g_Options.relaxedIncludes.end(), includeName) != g_Options.relaxedIncludes.end())
            continue;

        bool isFound = false;
        fs::path includeFile = path / includeName;
        if (fs::exists(includeFile))
            isFound = true;
        else {
            for (const fs::path& includePath : g_Options.includeDirs) {
                includeFile = includePath / includeName;
                if (fs::exists(includeFile)) {
                    isFound = true;
                    break;
                }
            }
        }

        // Metal standard headers are provided by the toolchain (only for native MSL, HLSL for "--metalFromDXIL" is tracked like DXIL)
        if (!isFound && g_Options.platform == METAL && !g_Options.metalFromDxil && matchResult[1] == "<" && IsMetalToolchainHeader(includeName))
            continue;

        if (!isFound) {
            Printf(RED "ERROR: Can't find include file '%s', included in:\n", PathToString(includeName).c_str());
            for (const fs::path& otherFile : callStack)
                Printf(RED "\t%s\n", PathToString(otherFile).c_str());

            return false;
        }

        fs::file_time_type dependencyTime;
        if (!GetHierarchicalUpdateTime(includeFile, callStack, dependencyTime))
            return false;

        hierarchicalUpdateTime = max(dependencyTime, hierarchicalUpdateTime);
    }

    callStack.pop_front();

    g_HierarchicalUpdateTimes[file] = hierarchicalUpdateTime;
    outTime = hierarchicalUpdateTime;

    return true;
}

static bool ProcessConfigLine(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime) {
    // Tokenize
    string lineCopy = line;
    vector<const char*> tokens;
    TokenizeConfigLine((char*)lineCopy.c_str(), tokens);

    // Parse config line
    ConfigLine configLine;
    if (!configLine.Parse((int32_t)tokens.size(), tokens.data())) {
        Printf(RED "%s(%u,0): ERROR: Can't parse config line!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    // DXBC: skip unsupported profiles
    string profile = configLine.profile;
    if (g_Options.platform == DXBC && (profile == "lib" || profile == "ms" || profile == "as"))
        return true;

    const char* compilerAliasName = nullptr;
    if (g_Options.platform == DXIL || g_Options.metalFromDxil)
        compilerAliasName = configLine.compilerDXIL;
    else if (g_Options.platform == SPIRV)
        compilerAliasName = configLine.compilerSPIRV;

    const CompilerAlias* compilerAlias = nullptr;
    if (compilerAliasName) {
        compilerAlias = FindCompilerAlias(compilerAliasName);
        if (!compilerAlias) {
            Printf(RED "%s(%u,0): ERROR: Compiler alias '%s' is not registered!\n",
                PathToString(g_Options.configFile).c_str(), lineIndex + 1, compilerAliasName);
            return false;
        }
    }

    CompilerType compilerType = compilerAlias ? compilerAlias->type : g_Options.compilerType;
    if (compilerType == COMPILER_SLANG && g_Options.metalFromDxil) {
        Printf(RED "%s(%u,0): ERROR: Slang is not supported by --metalFromDXIL!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
        return false;
    }

    if (compilerType == COMPILER_SLANG && g_Options.platform == SPIRV) {
        if (!GetSlangSpirvCapability(g_Options.vulkanVersion)) {
            Printf(RED
                "%s(%u,0): ERROR: Unsupported value '%s' for --vulkanVersion and Slang! Only '1.0', '1.1', "
                "'1.1spirv1.4', '1.2', '1.3' and '1.4' are supported.\n",
                PathToString(g_Options.configFile).c_str(), lineIndex + 1, g_Options.vulkanVersion);
            return false;
        }

        uint32_t minSpirvVersion = GetSlangProfileMinSpirvVersion(configLine.shaderModel);
        uint32_t maxSpirvVersion = GetVulkanMaxSpirvVersion(g_Options.vulkanVersion);
        if (minSpirvVersion > maxSpirvVersion) {
            Printf(RED "%s(%u,0): ERROR: Slang shader model '%s' requires SPIR-V %u.%u, which is not supported by Vulkan %s!\n",
                PathToString(g_Options.configFile).c_str(), lineIndex + 1, configLine.shaderModel,
                minSpirvVersion / 100, minSpirvVersion % 100, g_Options.vulkanVersion);
            return false;
        }
    }

    bool compilerAliasBuildSignatureChanged = false;
    if (compilerAlias) {
        auto [signatureIt, inserted] = g_CompilerAliasBuildSignatures.try_emplace(compilerAlias->name);
        BuildSignature& buildSignature = signatureIt->second;
        if (inserted) {
            buildSignature.path = GetCompilerAliasBuildSignaturePath(compilerAlias->name);
            buildSignature.value = GetCompilerBuildSignature(compilerAlias->path, compilerAlias->type);
            buildSignature.changed = !IsBuildSignatureCurrent(buildSignature.path, buildSignature.value);
        }

        compilerAliasBuildSignatureChanged = buildSignature.changed;
    }

    // Getting the sorted index of defines. While doing this, the value of defines are also get included in sorting problem
    // but it doesn't matter until two defines keys are identical which is not the case.
    vector<size_t> definesSortedIndices = ShaderMake::GetSortedConstantsIndices(configLine.defines);

    // Concatenate define strings, i.e. to get something, like: "A=1 B=0 C"
    string combinedDefines = "";
    for (size_t i = 0; i < configLine.defines.size(); i++) {
        size_t sortedIndex = definesSortedIndices[i];
        combinedDefines += configLine.defines[sortedIndex];
        if (i != configLine.defines.size() - 1)
            combinedDefines += " ";
    }

    // Compiled shader name
    fs::path shaderName = RemoveLeadingDotDots(configLine.source);
    if (configLine.outputSuffix)
        InsertSuffixBeforeExtensions(shaderName, configLine.outputSuffix);
    shaderName.replace_extension("");
    if (g_Options.flatten || configLine.outputDir) // Specifying -o <path> for a shader removes the original path
        shaderName = shaderName.filename();
    if (strcmp(configLine.entryPoint, "main"))
        InsertSuffixBeforeExtensions(shaderName, "_" + string(configLine.entryPoint));

    // Compiled permutation name
    fs::path permutationName = shaderName;
    if (!configLine.defines.empty()) {
        uint32_t permutationHash = HashToUint(hash<string>()(combinedDefines));

        char buf[16];
        snprintf(buf, sizeof(buf), "_%08X", permutationHash);

        InsertSuffixBeforeExtensions(permutationName, buf);
    }

    // Output directory
    fs::path outputDir = g_Options.outputDir;
    if (configLine.outputDir)
        outputDir /= configLine.outputDir;

    // Reject output collisions before workers can concurrently overwrite the same file.
    fs::path compilerOutputFile = outputDir / permutationName;
    compilerOutputFile += g_OutputExt;
    string outputKey = PathToString(fs::absolute(compilerOutputFile));
#ifdef _WIN32
    transform(outputKey.begin(), outputKey.end(), outputKey.begin(), [](char ch) { return (char)tolower((unsigned char)ch); });
#endif
    auto [outputIt, outputInserted] = g_OutputLines.emplace(outputKey, lineIndex + 1);
    if (!outputInserted) {
        Printf(RED "%s(%u,0): ERROR: Output '%s' is already produced by line %u!\n",
            PathToString(g_Options.configFile).c_str(), lineIndex + 1, outputKey.c_str(), outputIt->second);
        return false;
    }

    // Create intermediate output directories
    bool force = g_Options.force || compilerAliasBuildSignatureChanged;
    fs::path endPath = outputDir / shaderName.parent_path();
    if (g_Options.pdb && compilerType != COMPILER_SLANG && compilerType != COMPILER_METAL)
        endPath /= PDB_DIR;
    if (endPath.string() != "" && !fs::exists(endPath)) {
        fs::create_directories(endPath);
        force = true;
    }

    // Early out if no changes detected
    fs::file_time_type zero; // constructor sets to 0
    fs::file_time_type outputTime = zero;

    {
        fs::path outputFile = outputDir / permutationName;

        outputFile += g_OutputExt;
        if (g_Options.binary) {
            force |= !fs::exists(outputFile);
            if (!force) {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.header) {
            force |= !fs::exists(outputFile);
            if (!force) {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }
    }

    {
        fs::path outputFile = outputDir / shaderName;

        outputFile += g_OutputExt;
        if (g_Options.binaryBlob) {
            force |= !fs::exists(outputFile);
            if (!force) {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.headerBlob) {
            force |= !fs::exists(outputFile);
            if (!force) {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }
    }

    if (!force) {
        list<fs::path> callStack;
        fs::file_time_type sourceTime;
        fs::path sourceFile = g_Options.sourceDir / configLine.source;
        if (!GetHierarchicalUpdateTime(sourceFile, callStack, sourceTime))
            return false;

        sourceTime = max(sourceTime, configTime);

        // Global converter input files are tracked by the build signature
        if (g_Options.metalFromDxil) {
            for (const fs::path& file : GetMetalShaderConverterInputFiles(configLine.metalShaderConverterOptions)) {
                error_code errorCode;
                fs::file_time_type fileTime = fs::last_write_time(file, errorCode);
                if (!errorCode)
                    sourceTime = max(sourceTime, fileTime);
            }
        }

        if (outputTime > sourceTime)
            return true;
    }

    // Prepare a task
    string outputFileWithoutExt = PathToString(outputDir / permutationName);
    uint32_t optimizationLevel = configLine.optimizationLevel == USE_GLOBAL_OPTIMIZATION_LEVEL ? g_Options.optimizationLevel : configLine.optimizationLevel;
    optimizationLevel = min(optimizationLevel, 3u);

    TaskData& taskData = g_TaskData.emplace_back();
    taskData.source = configLine.source;
    taskData.entryPoint = configLine.entryPoint;
    taskData.profile = configLine.profile;
    taskData.shaderModel = configLine.shaderModel;
    fs::path compilerPath = compilerAlias ? compilerAlias->path : fs::path(g_Options.compiler);
    taskData.compiler = PathToString(fs::absolute(compilerPath));
    taskData.compilerType = compilerType;
    taskData.combinedDefines = combinedDefines;
    taskData.outputFileWithoutExt = outputFileWithoutExt;
    taskData.defines = configLine.defines;
    if (taskData.compilerType == COMPILER_SLANG && !HasDefine(g_Options.defines, "__SLANG__"))
        AddImplicitDefine(taskData.defines, "__SLANG__");
    if (g_Options.platform == SPIRV && !HasDefine(g_Options.defines, "__spirv__"))
        AddImplicitDefine(taskData.defines, "__spirv__");
    taskData.compilerOptions = configLine.compilerOptions;
    taskData.compilerOptionsDXIL = configLine.compilerOptionsDXIL;
    taskData.compilerOptionsSPIRV = configLine.compilerOptionsSPIRV;
    taskData.optimizationLevel = optimizationLevel;
    taskData.noRegShifts = configLine.noRegShifts;
    taskData.metalShaderConverterOptions = configLine.metalShaderConverterOptions;

    // Gather blobs
    if (g_Options.IsBlob()) {
        string blobName = PathToString(outputDir / shaderName);
        vector<BlobEntry>& entries = g_ShaderBlobs[blobName];

        BlobEntry entry;
        entry.permutationFileWithoutExt = outputFileWithoutExt;
        entry.combinedDefines = combinedDefines;
        entries.push_back(entry);
    }

    return true;
}

static bool ExpandPermutations(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime) {
    size_t opening = line.find('{');
    if (opening == string::npos)
        return ProcessConfigLine(lineIndex, line, configTime);

    size_t closing = line.find('}', opening);
    if (closing == string::npos) {
        Printf(RED "%s(%u,0): ERROR: Missing '}'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    size_t current = opening + 1;
    while (true) {
        size_t comma = line.find(',', current);
        if (comma == string::npos || comma > closing)
            comma = closing;

        string newConfig = line.substr(0, opening) + line.substr(current, comma - current) + line.substr(closing + 1);
        if (!ExpandPermutations(lineIndex, newConfig, configTime))
            return false;

        current = comma + 1;
        if (comma >= closing)
            break;
    }

    return true;
}

static bool CreateBlob(const string& blobName, const vector<BlobEntry>& entries, bool useTextOutput) {
    // Create output file
    string outputFile = blobName;
    outputFile += g_OutputExt;
    if (useTextOutput)
        outputFile += ".h";

    DataOutputContext outputContext(outputFile.c_str(), useTextOutput);
    if (!outputContext.stream) {
        Printf(RED "ERROR: Can't open output file '%s'!\n", outputFile.c_str());

        return false;
    }

    if (useTextOutput) {
        string name = GetShaderName(blobName);
        outputContext.WriteTextPreamble(name.c_str(), "");
    }

    ShaderMake::WriteFileCallback writeFileCallback = useTextOutput
        ? &DataOutputContext::WriteDataAsTextCallback
        : &DataOutputContext::WriteDataAsBinaryCallback;

    // Write "blob" header
    if (!ShaderMake::WriteFileHeader(writeFileCallback, &outputContext)) {
        Printf(RED "ERROR: Failed to write into output file '%s'!\n", outputFile.c_str());

        return false;
    }

    bool success = true;

    // Collect individual permutations
    for (const BlobEntry& entry : entries) {
        // Open compiled permutation file
        string file = entry.permutationFileWithoutExt + g_OutputExt;

        vector<uint8_t> fileData;
        if (ReadBinaryFile(file.c_str(), fileData)) {
            if (!ShaderMake::WritePermutation(writeFileCallback, &outputContext, entry.combinedDefines, fileData.data(), fileData.size())) {
                Printf(RED "ERROR: Failed to write a shader permutation into '%s'!\n", outputFile.c_str());
                success = false;
            }
        } else
            success = false;

        if (!success)
            break;
    }

    if (useTextOutput)
        outputContext.WriteTextEpilog();

    return success;
}

static void RemoveIntermediateBlobFiles(const vector<BlobEntry>& entries) {
    for (const BlobEntry& entry : entries) {
        string file = entry.permutationFileWithoutExt + g_OutputExt;
        fs::remove(file);
    }
}

static void SignalHandler(int32_t sig) {
    UNUSED(sig);

    g_Terminate = true;

    Printf(RED "Aborting...\n");
}

int32_t main(int32_t argc, const char** argv) {
    // Init timer
    Timer_Init();
    uint64_t start = Timer_GetTicks();

    // Set signal handler
    signal(SIGINT, SignalHandler);
#ifdef _WIN32
    signal(SIGBREAK, SignalHandler);
#endif

    // Parse command line
#if DEVMODE
    const char* self = argv[0];
#endif
    if (!g_Options.Parse(argc, argv))
        return 1;

    string buildSignature = GetBuildSignature();
    fs::path buildSignaturePath = GetBuildSignaturePath();
    bool buildSignatureChanged = !IsBuildSignatureCurrent(buildSignaturePath, buildSignature);
    g_Options.force |= buildSignatureChanged;
    bool buildSucceeded = true;

    { // Gather shader permutations
        fs::file_time_type configTime = fs::last_write_time(g_Options.configFile);
#if DEVMODE
        configTime = max(configTime, fs::last_write_time(self));
#endif

        ifstream configStream(g_Options.configFile);

        string line;
        line.reserve(256);

        vector<bool> blocks;
        blocks.push_back(true);

        for (uint32_t lineIndex = 0; getline(configStream, line); lineIndex++) {
            TrimConfigLine(line);

            // Skip an empty or commented line
            if (line.empty() || line[0] == '\n' || (line.size() > 1 && line[0] == '/' && line[1] == '/'))
                continue;

            // TODO: preprocessor supports "#ifdef MACRO / #if 1 / #if 0", "#else" and "#endif"
            size_t pos = line.find("#ifdef");
            if (pos != string::npos) {
                pos += 6;
                pos += line.substr(pos).find_first_not_of(' ');

                string define = line.substr(pos);
                bool state = blocks.back() && HasConfigDefine(define.c_str());

                blocks.push_back(state);
            } else if (line.find("#if 1") != string::npos)
                blocks.push_back(blocks.back());
            else if (line.find("#if 0") != string::npos)
                blocks.push_back(false);
            else if (line.find("#endif") != string::npos) {
                if (blocks.size() == 1)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#endif'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else
                    blocks.pop_back();
            } else if (line.find("#else") != string::npos) {
                if (blocks.size() < 2)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#else'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else if (blocks[blocks.size() - 2])
                    blocks.back() = !blocks.back();
            } else if (blocks.back()) {
                if (!ExpandPermutations(lineIndex, line, configTime))
                    return 1;
            }
        }
    }

    bool buildSignatureNeedsWrite = buildSignatureChanged || !g_TaskData.empty();
    if (buildSignatureNeedsWrite && !InvalidateBuildSignature(buildSignaturePath))
        return 1;

    for (const auto& [name, compilerBuildSignature] : g_CompilerAliasBuildSignatures) {
        UNUSED(name);
        if (compilerBuildSignature.changed && !InvalidateBuildSignature(compilerBuildSignature.path))
            return 1;
    }

    if (g_Options.pdb && any_of(g_TaskData.begin(), g_TaskData.end(), [](const TaskData& taskData) { return taskData.compilerType == COMPILER_SLANG; }))
        Printf(YELLOW "WARNING: ShaderMake does not support separate PDB output with Slang; --PDB enables debug information in the shader binary.\n");

    // Process tasks
    if (!g_TaskData.empty()) {
        Printf(WHITE "Compiling shaders using default compiler: %s\n", g_Options.compiler);
        for (const CompilerAlias& compilerAlias : g_Options.compilerAliases)
            Printf(WHITE "Compiler alias %s: %s\n", compilerAlias.name.c_str(), PathToString(compilerAlias.path).c_str());

        g_OriginalTaskCount = (uint32_t)g_TaskData.size();
        g_ProcessedTaskCount = 0;
        g_FailedTaskCount = 0;

        // Global retry budget for transient compilation subprocess launch failures.
        g_TaskRetryCount = g_Options.retryCount;

        uint32_t availableThreadNum = g_Options.jobs > 0 ? (uint32_t)g_Options.jobs : max(thread::hardware_concurrency(), 1u);
        uint32_t threadsNum = g_Options.serial ? 1 : min(availableThreadNum, g_OriginalTaskCount);

        vector<thread> threads;
        threads.reserve(threadsNum);
        for (uint32_t i = 0; i < threadsNum; i++)
            threads.emplace_back(ExeCompile);

        for (thread& worker : threads)
            worker.join();

        // If a fatal error or a termination request happened, don't proceed to the blob building.
        if (g_Terminate)
            return 1;

        // Dump shader blobs
        for (const auto& [blobName, blobEntries] : g_ShaderBlobs) {
            // If a blob contains one entry with no defines, just skip it.
            // The individual file's output name is the same as the blob, and we're done here.
            if (blobEntries.size() == 1 && blobEntries[0].combinedDefines.empty())
                continue;

            // Validate that the blob doesn't contain any shaders with empty defines.
            // In such case, that individual shader's output file is the same as the blob output file, which wouldn't work.
            // We could detect this condition earlier and work around it by renaming the shader output file, if necessary.
            bool invalidEntry = false;
            for (const auto& entry : blobEntries) {
                if (entry.combinedDefines.empty()) {
                    const string blobBaseName = fs::path(blobName).stem().generic_string();
                    Printf(RED "ERROR: Cannot create a blob for shader %s where some permutation(s) have no definitions!", blobBaseName.c_str());
                    invalidEntry = true;
                    break;
                }
            }

            if (invalidEntry) {
                buildSucceeded = false;
                if (g_Options.continueOnError)
                    continue;

                return 1;
            }

            if (g_Options.binaryBlob) {
                bool result = CreateBlob(blobName, blobEntries, false);
                buildSucceeded &= result;
                if (!result && !g_Options.continueOnError)
                    return 1;
            }

            if (g_Options.headerBlob) {
                bool result = CreateBlob(blobName, blobEntries, true);
                buildSucceeded &= result;
                if (!result && !g_Options.continueOnError)
                    return 1;
            }

            if (!g_Options.binary)
                RemoveIntermediateBlobFiles(blobEntries);
        }

        // Report failed tasks
        uint64_t end = Timer_GetTicks();
        double ms = Timer_ConvertTicksToMilliseconds(end - start);

        if (g_FailedTaskCount)
            Printf(YELLOW "WARNING: %u task(s) failed to complete (elapsed time %.2f ms)\n", g_FailedTaskCount.load(), ms);
        else
            Printf(WHITE "%u task(s) completed successfully (elapsed time %.2f ms)\n", g_OriginalTaskCount, ms);
    }

    if (!g_Terminate && !g_FailedTaskCount && buildSucceeded) {
        if (buildSignatureNeedsWrite && !WriteBuildSignature(buildSignaturePath, buildSignature))
            return 1;

        for (const auto& [name, compilerBuildSignature] : g_CompilerAliasBuildSignatures) {
            UNUSED(name);
            if (compilerBuildSignature.changed && !WriteBuildSignature(compilerBuildSignature.path, compilerBuildSignature.value))
                return 1;
        }
    }

    return (g_Terminate || g_FailedTaskCount || !buildSucceeded) ? 1 : 0;
}
