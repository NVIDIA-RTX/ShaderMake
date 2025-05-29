/*
Copyright (c) 2014-2023, NVIDIA CORPORATION. All rights reserved.

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

#include "argparse.h"
#include "ShaderBlob.h"

#include <sstream>
#include <fstream>
#include <map>
#include <vector>
#include <array>
#include <list>
#include <regex>
#include <thread>
#include <mutex>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstdarg>
#include <cstring>
#include <system_error>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <limits.h>
#endif

using namespace std;
namespace fs = filesystem;

#define _L(x)  __L(x)
#define __L(x) L ## x
#define UNUSED(x) ((void)(x))
#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

#define USE_GLOBAL_OPTIMIZATION_LEVEL 0xFF
#define SPIRV_SPACES_NUM 8
#define PDB_DIR "PDB"
#define BUF_SIZE 2048 // max line length
#define DEVMODE 1 // 1 forces recompilation if EXE changes

#ifdef _MSC_VER
    #define popen _popen
    #define pclose _pclose
    #define putenv _putenv
#endif

enum Platform : uint8_t
{
    DXBC,
    DXIL,
    SPIRV,

    PLATFORMS_NUM
};

struct Options
{
    vector<fs::path> includeDirs;
    vector<fs::path> relaxedIncludes;
    vector<string> defines;
    vector<string> spirvExtensions = {"SPV_EXT_descriptor_indexing", "KHR"};
    vector<string> compilerOptions;
    fs::path configFile;
    fs::path sourceDir;
    const char* projectName = "";
    const char* platformName = nullptr;
    const char* outputDir = nullptr;
    const char* shaderModel = "6_5";
    const char* vulkanVersion = "1.3";
    const char* compiler = nullptr;
    const char* outputExt = nullptr;
    const char* vulkanMemoryLayout = nullptr;
    uint32_t sRegShift = 100;
    uint32_t tRegShift = 200;
    uint32_t bRegShift = 300;
    uint32_t uRegShift = 400;
    uint32_t optimizationLevel = 3;
    int32_t retryCount = 10; // default 10 retries for compilation task sub-process failures
    Platform platform = DXBC;
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

    bool Parse(int32_t argc, const char** argv);

    inline bool IsBlob() const
    { return binaryBlob || headerBlob; }
};

struct ConfigLine
{
    vector<string> defines;
    const char* source = nullptr;
    const char* entryPoint = "main";
    const char* profile = nullptr;
    const char* outputDir = nullptr;
    const char* outputSuffix = nullptr;
    const char* shaderModel = nullptr;

    uint32_t optimizationLevel = USE_GLOBAL_OPTIMIZATION_LEVEL;

    bool Parse(int32_t argc, const char** argv);
};

struct TaskData
{
    vector<string> defines;
    string source;
    string entryPoint;
    string profile;
    string shaderModel;
    string outputFileWithoutExt;
    string combinedDefines;
    uint32_t optimizationLevel = 3;
};

struct BlobEntry
{
    string permutationFileWithoutExt;
    string combinedDefines;
};

Options g_Options;
map<fs::path, fs::file_time_type> g_HierarchicalUpdateTimes;
map<string, vector<BlobEntry>> g_ShaderBlobs;
vector<TaskData> g_TaskData;
mutex g_TaskMutex;
atomic<uint32_t> g_ProcessedTaskCount;
atomic<uint32_t> g_PrevProgress;
atomic<int32_t> g_TaskRetryCount;
atomic<bool> g_Terminate = false;
atomic<uint32_t> g_FailedTaskCount = 0;
uint32_t g_OriginalTaskCount;
const char* g_OutputExt = nullptr;

std::array<const char*, 3> g_PlatformNames = {
    "DXBC",
    "DXIL",
    "SPIRV",
};

std::array<const char*, 3> g_PlatformExts = {
    ".dxbc",
    ".dxil",
    ".spirv",
};

std::array<const char*, 3> g_PlatformSlangTargets = {
    "dxbc",
    "dxil",
    "spirv",
};

#if 1
    #define RED "\x1b[31m"
    #define GRAY "\x1b[90m"
    #define WHITE "\x1b[0m"
    #define GREEN "\x1b[32m"
    #define YELLOW "\x1b[33m"
#else
    #define RED ""
    #define GRAY ""
    #define WHITE ""
    #define GREEN ""
    #define YELLOW ""
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

inline uint32_t HashToUint(size_t hash)
{ return uint32_t(hash) ^ (uint32_t(hash >> 32)); }

inline string PathToString(fs::path path)
{ return path.lexically_normal().make_preferred().string(); }

inline wstring AnsiToWide(const string& s)
{ return wstring(s.begin(), s.end()); }

inline fs::path RemoveLeadingDotDots(const fs::path& path)
{
    auto it = path.begin();
    while (*it == ".." && it != path.end())
        ++it;

    fs::path result;
    while (it != path.end())
    {
        result = result / *it;
        ++it;
    }

    return result;
}

inline bool IsSpace(char ch)
{ return strchr(" \t\r\n", ch) != nullptr; }

inline bool HasRepeatingSpace(char a, char b)
{ return (a == b) && a == ' '; }

inline string EscapePath(const string& s)
{
    if (s.find(' ') != string::npos)
        return "\"" + s + "\"";

    return s;
}

inline void TrimConfigLine(string& s)
{
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

void TokenizeConfigLine(char* in, vector<const char*>& tokens)
{
    char* out = in;
    char* token = out;

    // Some magic to correctly tokenize spaces in ""
    bool isString = false;
    while (*in)
    {
        if (*in == '"')
            isString = !isString;
        else if (*in == ' ' && !isString)
        {
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

uint32_t GetFileLength(FILE* stream)
{
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

void Printf(const char* format, ...)
{
    va_list argptr;
    va_start(argptr, format);

    // Remove embedded colors if colorization is off
    char fixedFormat[BUF_SIZE]; // TODO: let's assume that we always fit
    if (!g_Options.colorize)
    {
        const char* in = format;
        char* out = fixedFormat;

        while (*in)
        {
            if (*in == '\x1b')
                while( *in++ != 'm' );

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

string GetShaderName(const fs::path& path)
{
    string name = path.filename().string();
    replace(name.begin(), name.end(), '.', '_');
    name += "_" + string(g_PlatformExts[g_Options.platform] + 1);

    return "g_" + name;
}

// A class that is used to write a code blob as binary or C-string
class DataOutputContext
{
public:
    FILE* stream = nullptr;

    DataOutputContext(const char* file, bool textMode)
    {
        stream = fopen(file, textMode ? "w": "wb");
        if (!stream)
            Printf(RED "ERROR: Can't open file '%s' for writing!\n", file);
    }

    ~DataOutputContext()
    {
        if (stream)
        {
            fclose(stream);
            stream = nullptr;
        }
    }

    bool WriteDataAsText(const void* data, size_t size)
    {
        for (size_t i = 0; i < size; i++)
        {
            uint8_t value = ((const uint8_t*)data)[i];

            if (m_lineLength > 128)
            {
                fprintf(stream, "\n    ");
                m_lineLength = 0;
            }

            fprintf(stream, "%u,", value);

            if (value < 10)
                m_lineLength += 3;
            else if (value < 100)
                m_lineLength += 4;
            else
                m_lineLength += 5;
        }

        return true;
    }

    void WriteTextPreamble(const char* shaderName, const std::string& combinedDefines)
    {
        fprintf(stream, "// {%s}\n", combinedDefines.c_str());
        fprintf(stream, "const uint8_t %s[] = {", shaderName);
    }

    void WriteTextEpilog()
    { fprintf(stream, "\n};\n"); }

    bool WriteDataAsBinary(const void* data, size_t size)
    {
        if (size == 0)
            return true;

        return fwrite(data, size, 1, stream) == 1;
    }

    // For use as a callback in "WriteFileHeader" and "WritePermutation" functions
    static bool WriteDataAsTextCallback(const void* data, size_t size, void* context)
    { return ((DataOutputContext*)context)->WriteDataAsText(data, size); }

    static bool WriteDataAsBinaryCallback(const void* data, size_t size, void* context)
    { return ((DataOutputContext*)context)->WriteDataAsBinary(data, size); }

private:
    uint32_t m_lineLength = 129;
};

void DumpShader(const TaskData& taskData, const uint8_t* data, size_t dataSize)
{
    string file = taskData.outputFileWithoutExt + g_OutputExt;

    if (g_Options.binary || g_Options.binaryBlob || (g_Options.headerBlob && !taskData.combinedDefines.empty()))
    {
        DataOutputContext context(file.c_str(), false);
        if (!context.stream)
            return;

        context.WriteDataAsBinary(data, dataSize);
    }

    if (g_Options.header || (g_Options.headerBlob && taskData.combinedDefines.empty()))
    {
        DataOutputContext context((file + ".h").c_str(), true);
        if (!context.stream)
            return;

        string shaderName = GetShaderName(taskData.outputFileWithoutExt);
        context.WriteTextPreamble(shaderName.c_str(), taskData.combinedDefines);
        context.WriteDataAsText(data, dataSize);
        context.WriteTextEpilog();
    }
}

void UpdateProgress(const TaskData& taskData, bool isSucceeded, bool willRetry, const char* message)
{
    // IMPORTANT: do not split into several "Printf" calls because multi-threading access to the console can mess up the order
    if (isSucceeded)
    {
        float progress = 100.0f * float(++g_ProcessedTaskCount) / float(g_OriginalTaskCount);

        if (message && *message != '\0')
        {
            Printf(YELLOW "(%5.1f%%) %s %s %s {%s} {%s}\n%s",
                progress, g_Options.projectName, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str(),
                message);
        }
        else
        {
            if (g_Options.compactProgress)
            {
                uint32_t progressSnapped = (uint32_t(progress + 0.5f) / 10) * 10;
                if (progressSnapped != g_PrevProgress)
                {
                    Printf(GREEN "(%3u%%)" GRAY " %s %s\n", progressSnapped, g_Options.projectName, g_Options.platformName);
                    g_PrevProgress = progressSnapped;
                }
            }
            else
            {
                Printf(GREEN "(%5.1f%%)" GRAY " %s %s" WHITE " %s" GRAY " {%s}" WHITE " {%s}\n",
                    progress, g_Options.projectName, g_Options.platformName,
                    taskData.source.c_str(),
                    taskData.entryPoint.c_str(),
                    taskData.combinedDefines.c_str());
            }
        }
    }
    else
    {
        // If retrying, requeue the task and try again without counting failure or terminating
        if (willRetry)
        {
            Printf(YELLOW "( RETRY-QUEUED ) %s %s %s {%s} {%s}\n",
                g_Options.projectName, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str());

            lock_guard<mutex> guard(g_TaskMutex);
            g_TaskData.push_back(taskData);

            --g_TaskRetryCount;
        }
        else
        {
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

double Timer_ConvertTicksToMilliseconds(uint64_t ticks)
{ return (double)ticks * g_TicksToMilliseconds; }

void Timer_Init()
{
#ifdef _WIN32
    uint64_t ticksPerSecond = 1;
    QueryPerformanceFrequency((LARGE_INTEGER*)&ticksPerSecond);

    g_TicksToMilliseconds = 1000.0 / ticksPerSecond;
#else
    g_TicksToMilliseconds = 1.0 / 1000000.0;
#endif
}

uint64_t Timer_GetTicks()
{
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

int32_t AddInclude(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->includeDirs.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int32_t AddGlobalDefine(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->defines.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int32_t AddRelaxedInclude(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->relaxedIncludes.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int32_t AddSpirvExtension(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->spirvExtensions.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int32_t AddCompilerOptions(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->compilerOptions.push_back(*(const char**)option->value); UNUSED(self); return 0; }

bool Options::Parse(int32_t argc, const char** argv)
{
    const char* config = nullptr;
    const char* unused = nullptr; // storage for callbacks
    const char* srcDir = "";
    bool ignoreConfigDir = false;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Required options:"),
            OPT_STRING('p', "platform", &platformName, "DXBC, DXIL or SPIRV", nullptr, 0, 0),
            OPT_STRING('c', "config", &config, "Configuration file with the list of shaders to compile", nullptr, 0, 0),
            OPT_STRING('o', "out", &outputDir, "Output directory", nullptr, 0, 0),
            OPT_BOOLEAN('b', "binary", &binary, "Output binary files", nullptr, 0, 0),
            OPT_BOOLEAN('h', "header", &header, "Output header files", nullptr, 0, 0),
            OPT_BOOLEAN('B', "binaryBlob", &binaryBlob, "Output binary blob files", nullptr, 0, 0),
            OPT_BOOLEAN('H', "headerBlob", &headerBlob, "Output header blob files", nullptr, 0, 0),
            OPT_STRING(0, "compiler", &compiler, "Path to a FXC/DXC/Slang compiler executable", nullptr, 0, 0),
        OPT_GROUP("Compiler settings:"),
            OPT_STRING('m', "shaderModel", &shaderModel, "Shader model for DXIL/SPIRV (always SM 5.0 for DXBC) in 'X_Y' format", nullptr, 0, 0),
            OPT_INTEGER('O', "optimization", &optimizationLevel, "Optimization level 0-3 (default = 3, disabled = 0)", nullptr, 0, 0),
            OPT_STRING('X', "compilerOptions", &unused, "Custom command line options for the compiler, separated by spaces", AddCompilerOptions, (intptr_t)this, 0),
            OPT_BOOLEAN(0, "WX", &warningsAreErrors, "Maps to '-WX' DXC/FXC option: warnings are errors", nullptr, 0, 0),
            OPT_BOOLEAN(0, "allResourcesBound", &allResourcesBound, "Maps to '-all_resources_bound' DXC/FXC option: all resources bound", nullptr, 0, 0),
            OPT_BOOLEAN(0, "PDB", &pdb, "Output PDB files in 'out/PDB/' folder", nullptr, 0, 0),
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
            OPT_STRING(0, "outputExt", &outputExt, "Extension for output files, default is one of .dxbc, .dxil, .spirv", nullptr, 0, 0),
            OPT_BOOLEAN(0, "serial", &serial, "Disable multi-threading", nullptr, 0, 0),
            OPT_BOOLEAN(0, "flatten", &flatten, "Flatten source directory structure in the output directory", nullptr, 0, 0),
            OPT_BOOLEAN(0, "continue", &continueOnError, "Continue compilation if an error is occured", nullptr, 0, 0),
            OPT_BOOLEAN(0, "colorize", &colorize, "Colorize console output", nullptr, 0, 0),
            OPT_BOOLEAN(0, "verbose", &verbose, "Print commands before they are executed", nullptr, 0, 0),
            OPT_INTEGER(0, "retryCount", &retryCount, "Retry count for compilation task sub-process failures", nullptr, 0, 0),
            OPT_BOOLEAN(0, "ignoreConfigDir", &ignoreConfigDir, "Use 'current dir' instead of 'config dir' as parent path for relative dirs", nullptr, 0, 0),
            OPT_BOOLEAN(0, "compactProgress", &compactProgress, "Compact compilation progress reporting", nullptr, 0, 0),
        OPT_GROUP("SPIRV options:"),
            OPT_STRING(0, "vulkanMemoryLayout", &vulkanMemoryLayout, "Maps to '-fvk-use-<VALUE>-layout' DXC options: dx, gl, scalar", nullptr, 0, 0),
            OPT_STRING(0, "vulkanVersion", &vulkanVersion, "Vulkan environment version, maps to '-fspv-target-env' (default = 1.3)", nullptr, 0, 0),
            OPT_STRING(0, "spirvExt", &unused, "Maps to '-fspv-extension' option: add SPIR-V extension permitted to use", AddSpirvExtension, (intptr_t)this, 0),
            OPT_INTEGER(0, "sRegShift", &sRegShift, "SPIRV: register shift for sampler (s#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "tRegShift", &tRegShift, "SPIRV: register shift for texture (t#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "bRegShift", &bRegShift, "SPIRV: register shift for constant (b#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "uRegShift", &uRegShift, "SPIRV: register shift for UAV (u#) resources", nullptr, 0, 0),
            OPT_BOOLEAN(0, "noRegShifts", &noRegShifts, "Don't specify any register shifts for the compiler", nullptr, 0, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "ShaderMake.exe -p {DXBC|DXIL|SPIRV} --binary [--header --blob] -c \"path/to/config\"\n"
        "\t-o \"path/to/output\" --compiler \"path/to/compiler\" [other options]\n"
        "\t-D DEF1 -D DEF2=1 ... -I \"path1\" -I \"path2\" ...",
        nullptr
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nMulti-threaded shader compiling & processing tool");
    argparse_parse(&argparse, argc, argv);

    if (!config)
    {
        Printf(RED "ERROR: Config file not specified!\n");
        return false;
    }

    if (!fs::exists(config))
    {
        Printf(RED "ERROR: Config file '%s' does not exist!\n", config);
        return false;
    }

    if (!outputDir)
    {
        Printf(RED "ERROR: Output directory not specified!\n");
        return false;
    }

    if (!binary && !header && !binaryBlob && !headerBlob)
    {
        Printf(RED "ERROR: One of 'binary', 'header', 'binaryBlob' or 'headerBlob' must be set!\n");
        return false;
    }
    if (!platformName)
    {
        Printf(RED "ERROR: Platform not specified!\n");
        return false;
    }

    if (!compiler)
    {
        Printf(RED "ERROR: Compiler not specified!\n");
        return false;
    }

    if (!fs::exists(compiler))
    {
        Printf(RED "ERROR: Compiler '%s' does not exist!\n", compiler);
        return false;
    }

    if (strlen(shaderModel) != 3 || strstr(shaderModel, "."))
    {
        Printf(RED "ERROR: Shader model ('%s') must have format 'X_Y'!\n", shaderModel);
        return false;
    }

    // Platform
    uint32_t i = 0;
    for (; i < PLATFORMS_NUM; i++)
    {
        if (!strcmp(platformName, g_PlatformNames[i]))
        {
            platform = (Platform)i;
            break;
        }
    }
    if (i == PLATFORMS_NUM)
    {
        Printf(RED "ERROR: Unrecognized platform '%s'!\n", platformName);
        return false;
    }

    if (outputExt)
        g_OutputExt = outputExt;
    else
        g_OutputExt = g_PlatformExts[platform];

    if (g_Options.vulkanMemoryLayout && platform != SPIRV)
    {
        Printf(RED "ERROR: --vulkanMemoryLayout is only supported for SPIRV target!\n");
        return false;
    }

    if (g_Options.vulkanMemoryLayout &&
        strcmp(g_Options.vulkanMemoryLayout, "dx") != 0 &&
        strcmp(g_Options.vulkanMemoryLayout, "gl") != 0 &&
        strcmp(g_Options.vulkanMemoryLayout, "scalar") != 0)
    {
        if (g_Options.slang && (strcmp(g_Options.vulkanMemoryLayout, "dx") == 0))
        {
            Printf(RED "ERROR: Unsupported value '%s' for --vulkanMemoryLayout! Only 'gl' and 'scalar' are supported for Slang.\n",
                g_Options.vulkanMemoryLayout);
        }
        else {
            Printf(RED "ERROR: Unsupported value '%s' for --vulkanMemoryLayout! Only 'dx', 'gl' and 'scalar' are supported.\n",
                g_Options.vulkanMemoryLayout);
        }
        return false;
    }

    if (g_Options.retryCount < 0)
    {
        Printf(RED "ERROR: --retryCount must be greater than or equal to 0.\n");
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
    if (fsSrcDir.is_relative())
    {
        if (ignoreConfigDir)
            g_Options.sourceDir = fs::path(cd) / fsSrcDir;
        else
            g_Options.sourceDir = g_Options.configFile.parent_path() / fsSrcDir;
    }
    else
        g_Options.sourceDir = fsSrcDir;

    for (fs::path& path : includeDirs)
    {
        if (path.is_relative())
        {
            if (ignoreConfigDir)
                path = fs::path(cd) / path;
            else
                path = configFile.parent_path() / path;
        }
    }

    return true;
}

int32_t AddLocalDefine(struct argparse* self, const struct argparse_option* option)
{ ((ConfigLine*)(option->data))->defines.push_back(*(const char**)option->value); UNUSED(self); return 0; }

bool ConfigLine::Parse(int32_t argc, const char** argv)
{
    source = argv[0];

    const char* unused = nullptr; // storage for the callback
    struct argparse_option options[] = {
        OPT_STRING('T', "profile", &profile, "Shader profile", nullptr, 0, 0),
        OPT_STRING('E', "entryPoint", &entryPoint, "(Optional) entry point", nullptr, 0, 0),
        OPT_STRING('D', "define", &unused, "(Optional) define(s) in forms 'M=value' or 'M'", AddLocalDefine, (intptr_t)this, 0),
        OPT_STRING('o', "output", &outputDir, "(Optional) output subdirectory", nullptr, 0, 0),
        OPT_INTEGER('O', "optimization", &optimizationLevel, "(Optional) optimization level", nullptr, 0, 0),
        OPT_STRING('s', "outputSuffix", &outputSuffix, "(Optional) suffix to add before extension after filename", nullptr, 0, 0),
        OPT_STRING('m', "shaderModel", &shaderModel, "(Optional) shader model for DXIL/SPIRV (always SM 5.0 for DXBC) in 'X_Y' format", nullptr, 0, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "path/to/shader -T profile [-E entry -O{0|1|2|3} -o \"output/subdirectory\" -s \"suffix\" -m 6_5 -D DEF1={0,1} -D DEF2={0,1,2} -D DEF3 ...]",
        nullptr
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nConfiguration options for a shader");
    argparse_parse(&argparse, argc, argv);

    if (!shaderModel)
        shaderModel = g_Options.shaderModel;

    // If there are some non-option elements in the config line, they will remain in the argv array.
    if (argv[0])
    {
        Printf(RED "ERROR: Unrecognized element in the config line: '%s'!\n", argv[0]);
        return false;
    }

    if (!profile)
    {
        Printf(RED "ERROR: Shader target not specified!\n");
        return false;
    }

    if (strlen(shaderModel) != 3 || strstr(shaderModel, "."))
    {
        Printf(RED "ERROR: Shader model ('%s') must have format 'X_Y'!\n", shaderModel);
        return false;
    }

    return true;
}

bool ReadBinaryFile(const char* file, vector<uint8_t>& outData)
{
    FILE* stream = fopen(file, "rb");
    if (!stream)
    {
        Printf(RED "ERROR: Can't open file '%s'!\n", file);
        return false;
    }

    uint32_t const binarySize = GetFileLength(stream);
    if (binarySize == 0)
    {
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

void ExeCompile()
{
    static const char* optimizationLevelRemap[] = {
        " -Od",
        " -O1",
        " -O2",
        " -O3",
    };

    while (!g_Terminate)
    {
        // Getting a task in the current thread
        TaskData taskData;
        {
            lock_guard<mutex> guard(g_TaskMutex);
            if (g_TaskData.empty())
                return;

            taskData = g_TaskData.back();
            g_TaskData.pop_back();
        }

        bool convertBinaryOutputToHeader = false;
        string outputFile = taskData.outputFileWithoutExt + g_OutputExt;

        // Building command line
        ostringstream cmd;
        {
            #ifdef _WIN32 // workaround for Windows
                cmd << "%COMPILER%";
            #else
                cmd << "$COMPILER";
            #endif

            if (g_Options.slang)
            {
                if (g_Options.header || (g_Options.headerBlob && taskData.combinedDefines.empty()))
                    convertBinaryOutputToHeader = true;

                // Slang defaults to slang language mode unless -lang <other language> sets something else.
                // For HLSL compatibility mode:
                //    - use -lang hlsl to set language mode to HLSL
                //    - use -unscoped-enums so Slang doesn't require all enums to be scoped
                if (g_Options.slangHlsl)
                {
                    // Language mode: hlsl
                    cmd << " -lang hlsl";

                    // Treat enums as unscoped
                    cmd << " -unscoped-enum";
                }

                // Profile
                cmd << " -profile " << taskData.profile << "_" << taskData.shaderModel;

                // Target/platform
                cmd << " -target " << g_PlatformSlangTargets[g_Options.platform];

                // Output
                cmd << " -o " << EscapePath(outputFile);

                // Entry point
                if (taskData.profile != "lib")
                {
                    // Don't specify entry if profile is lib_*, Slang will use the entry point currently
                    cmd << " -entry " << taskData.entryPoint;
                }

                // Defines
                for (const string& define : taskData.defines)
                    cmd << " -D " << define;

                for (const string& define : g_Options.defines)
                    cmd << " -D " << define;

                // Include directories
                for (const fs::path& dir : g_Options.includeDirs)
                    cmd << " -I " << EscapePath(dir.string());

                // Optimization level
                cmd << " -O" << taskData.optimizationLevel;

                // Warnings as errors
                if (g_Options.warningsAreErrors)
                    cmd << " -warnings-as-errors";

                // Matrix layout
                if (g_Options.matrixRowMajor)
                    cmd << " -matrix-layout-row-major";
                else
                    cmd << " -matrix-layout-column-major";

                if (g_Options.platform == SPIRV)
                {
                    // Uses the entrypoint name from the source instead of 'main' in the SPIRV output
                    cmd << " -fvk-use-entrypoint-name";

                    if (g_Options.vulkanMemoryLayout)
                    {
                        if (strcmp(g_Options.vulkanMemoryLayout, "scalar") == 0)
                            cmd << " -force-glsl-scalar-layout";
                        else if (strcmp(g_Options.vulkanMemoryLayout, "gl") == 0)
                            cmd << " -fvk-use-gl-layout";
                    }

                    if (!g_Options.noRegShifts)
                    {
                        for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++)
                        {
                            cmd << " -fvk-s-shift " << g_Options.sRegShift << " " << space;
                            cmd << " -fvk-t-shift " << g_Options.tRegShift << " " << space;
                            cmd << " -fvk-b-shift " << g_Options.bRegShift << " " << space;
                            cmd << " -fvk-u-shift " << g_Options.uRegShift << " " << space;
                        }
                    }
                }

                // Custom options
                for (const string& options : g_Options.compilerOptions)
                    cmd << " " << options;
            }
            else
            {
                cmd << " -nologo";

                // Output file
                if (g_Options.binary || g_Options.binaryBlob || (g_Options.headerBlob && !taskData.combinedDefines.empty()))
                    cmd << " -Fo " << EscapePath(outputFile);
                if (g_Options.header || (g_Options.headerBlob && taskData.combinedDefines.empty()))
                {
                    string name = GetShaderName(taskData.outputFileWithoutExt);

                    cmd << " -Fh " << EscapePath(outputFile) << ".h";
                    cmd << " -Vn " << name;
                }

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

                uint32_t shaderModelIndex = (taskData.shaderModel[0] - '0') * 10 + (taskData.shaderModel[2] - '0');
                if (g_Options.platform != DXBC && shaderModelIndex >= 62)
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

                if (g_Options.platform == SPIRV)
                {
                    cmd << " -spirv";

                    cmd << " -fspv-target-env=vulkan" << g_Options.vulkanVersion;

                    if (g_Options.vulkanMemoryLayout)
                        cmd << " -fvk-use-" << g_Options.vulkanMemoryLayout << "-layout";

                    for (const string& ext : g_Options.spirvExtensions)
                        cmd << " -fspv-extension=" << ext;

                    if (!g_Options.noRegShifts)
                    {
                        for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++)
                        {
                            cmd << " -fvk-s-shift " << g_Options.sRegShift << " " << space;
                            cmd << " -fvk-t-shift " << g_Options.tRegShift << " " << space;
                            cmd << " -fvk-b-shift " << g_Options.bRegShift << " " << space;
                            cmd << " -fvk-u-shift " << g_Options.uRegShift << " " << space;
                        }
                    }
                }
                else // Not supported by SPIRV gen
                {
                    if (g_Options.stripReflection)
                        cmd << " -Qstrip_reflect";

                    if (g_Options.pdb)
                    {
                        fs::path pdbPath = fs::path(outputFile).parent_path() / PDB_DIR;
                        cmd << " -Fd " << EscapePath(pdbPath.string() + "/"); // only binary code affects hash
                    }
                }

                // Custom options
                for (const string& options : g_Options.compilerOptions)
                    cmd << " " << options;
            }

            // Source file
            fs::path sourceFile = g_Options.sourceDir / taskData.source;
            cmd << " " << EscapePath(sourceFile.string());
        }

        cmd << " 2>&1";

        // Debug output
        if (g_Options.verbose)
            Printf(WHITE "%s\n", cmd.str().c_str());

        // Compiling the shader
        ostringstream msg;
        FILE* pipe = popen(cmd.str().c_str(), "r");

        bool isSucceeded = false;
        bool willRetry = false;
        if (pipe)
        {
            char buf[BUF_SIZE];
            while (fgets(buf, sizeof(buf), pipe))
            {
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
            bool commandShellError = (WIFEXITED(result) && WEXITSTATUS(result) == 127);
#endif

            if (result == 0)
                isSucceeded = true;

            // Retry if count > 0 and failed to execute child sub-process or command shell (posix only)
            else if (g_TaskRetryCount > 0 && (childProcessError || commandShellError))
                willRetry = true;
        }

        // Slang cannot produce .h files directly, so we convert its binary output to .h here if needed
        if (isSucceeded && convertBinaryOutputToHeader)
        {
            vector<uint8_t> buffer;
            if (ReadBinaryFile(outputFile.c_str(), buffer))
            {
                string headerFile = taskData.outputFileWithoutExt + g_OutputExt + ".h";
                DataOutputContext context(headerFile.c_str(), true);
                if (context.stream)
                {
                    string shaderName = GetShaderName(taskData.outputFileWithoutExt);
                    context.WriteTextPreamble(shaderName.c_str(), taskData.combinedDefines);
                    context.WriteDataAsText(buffer.data(), buffer.size());
                    context.WriteTextEpilog();

                    // Delete the binary file if it's not requested
                    if (!g_Options.binary)
                        fs::remove(outputFile);
                }
                else
                {
                    Printf(RED "ERROR: Failed to open file '%s' for writing!\n", headerFile.c_str());
                    isSucceeded = false;
                }
            }
            else
                isSucceeded = false;
        }

        // Update progress
        UpdateProgress(taskData, isSucceeded, willRetry, msg.str().c_str());
    }
}

//=====================================================================================================================
// MAIN
//=====================================================================================================================

bool GetHierarchicalUpdateTime(const fs::path& file, list<fs::path>& callStack, fs::file_time_type& outTime)
{
    static const basic_regex<char> includePattern("\\s*#include\\s+[\"<]([^>\"]+)[>\"].*");

    auto found = g_HierarchicalUpdateTimes.find(file);
    if (found != g_HierarchicalUpdateTimes.end())
    {
        outTime = found->second;

        return true;
    }

    ifstream stream(file);
    if (!stream.is_open())
    {
        Printf(RED "ERROR: Can't open file '%s', included in:\n", PathToString(file).c_str());
        for (const fs::path& otherFile : callStack)
            Printf(RED "\t%s\n", PathToString(otherFile).c_str());

        return false;
    }

    callStack.push_front(file);

    fs::path path = file.parent_path();
    fs::file_time_type hierarchicalUpdateTime = fs::last_write_time(file);

    for (string line; getline(stream, line);)
    {
        match_results<const char*> matchResult;
        regex_match(line.c_str(), matchResult, includePattern);
        if (matchResult.empty())
            continue;

        fs::path includeName = string(matchResult[1]);
        if (find(g_Options.relaxedIncludes.begin(), g_Options.relaxedIncludes.end(), includeName) != g_Options.relaxedIncludes.end())
            continue;

        bool isFound = false;
        fs::path includeFile = path / includeName;
        if (fs::exists(includeFile))
            isFound = true;
        else
        {
            for (const fs::path& includePath : g_Options.includeDirs)
            {
                includeFile = includePath / includeName;
                if (fs::exists(includeFile))
                {
                    isFound = true;
                    break;
                }
            }
        }

        if (!isFound)
        {
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

bool ProcessConfigLine(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime)
{
    // Tokenize
    string lineCopy = line;
    vector<const char*> tokens;
    TokenizeConfigLine((char*)lineCopy.c_str(), tokens);

    // Parse config line
    ConfigLine configLine;
    if (!configLine.Parse((int32_t)tokens.size(), tokens.data()))
    {
        Printf(RED "%s(%u,0): ERROR: Can't parse config line!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    // DXBC: skip unsupported profiles
    string profile = configLine.profile;
    if (g_Options.platform == DXBC && (profile == "lib" || profile == "ms" || profile == "as"))
        return true;

    // Getting the sorted index of defines. While doing this, the value of defines are also get included in sorting problem
    // but it doesn't matter until two defines keys are identical which is not the case.
    vector<size_t> definesSortedIndices = ShaderMake::GetSortedConstantsIndices(configLine.defines);

    // Concatenate define strings, i.e. to get something, like: "A=1 B=0 C"
    string combinedDefines = "";
    for (size_t i = 0; i < configLine.defines.size(); i++)
    {
        size_t sortedIndex = definesSortedIndices[i];
        combinedDefines += configLine.defines[sortedIndex];
        if (i != configLine.defines.size() - 1 )
            combinedDefines += " ";
    }

    // Compiled shader name
    fs::path shaderName = RemoveLeadingDotDots(configLine.source);
    shaderName.replace_extension("");
    if (g_Options.flatten || configLine.outputDir) // Specifying -o <path> for a shader removes the original path
        shaderName = shaderName.filename();
    if (strcmp(configLine.entryPoint, "main"))
        shaderName += "_" + string(configLine.entryPoint);
    if(configLine.outputSuffix)
        shaderName += string(configLine.outputSuffix);

    // Compiled permutation name
    fs::path permutationName = shaderName;
    if (!configLine.defines.empty())
    {
        uint32_t permutationHash = HashToUint(hash<string>()(combinedDefines));

        char buf[16];
        snprintf(buf, sizeof(buf), "_%08X", permutationHash);

        permutationName += buf;
    }

    // Output directory
    fs::path outputDir = g_Options.outputDir;
    if (configLine.outputDir)
        outputDir /= configLine.outputDir;

    // Create intermediate output directories
    bool force = g_Options.force;
    fs::path endPath = outputDir / shaderName.parent_path();
    if (g_Options.pdb)
        endPath /= PDB_DIR;
    if (endPath.string() != "" && !fs::exists(endPath))
    {
        fs::create_directories(endPath);
        force = true;
    }

    // Early out if no changes detected
    fs::file_time_type zero; // constructor sets to 0
    fs::file_time_type outputTime = zero;

    {
        fs::path outputFile = outputDir / permutationName;

        outputFile += g_OutputExt;
        if (g_Options.binary)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.header)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
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
        if (g_Options.binaryBlob)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.headerBlob)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }
    }

    if (!force)
    {
        list<fs::path> callStack;
        fs::file_time_type sourceTime;
        fs::path sourceFile = g_Options.sourceDir / configLine.source;
        if (!GetHierarchicalUpdateTime(sourceFile, callStack, sourceTime))
            return false;

        sourceTime = max(sourceTime, configTime);
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
    taskData.combinedDefines = combinedDefines;
    taskData.outputFileWithoutExt = outputFileWithoutExt;
    taskData.defines = configLine.defines;
    taskData.optimizationLevel = optimizationLevel;

    // Gather blobs
    if (g_Options.IsBlob())
    {
        string blobName = PathToString(outputDir / shaderName);
        vector<BlobEntry>& entries = g_ShaderBlobs[blobName];

        BlobEntry entry;
        entry.permutationFileWithoutExt = outputFileWithoutExt;
        entry.combinedDefines = combinedDefines;
        entries.push_back(entry);
    }

    return true;
}

bool ExpandPermutations(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime)
{
    size_t opening = line.find('{');
    if (opening == string::npos)
        return ProcessConfigLine(lineIndex, line, configTime);

    size_t closing = line.find('}', opening);
    if (closing == string::npos)
    {
        Printf(RED "%s(%u,0): ERROR: Missing '}'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    size_t current = opening + 1;
    while (true)
    {
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

bool CreateBlob(const string& blobName, const vector<BlobEntry>& entries, bool useTextOutput)
{
    // Create output file
    string outputFile = blobName;
    outputFile += g_OutputExt;
    if (useTextOutput)
        outputFile += ".h";

    DataOutputContext outputContext(outputFile.c_str(), useTextOutput);
    if (!outputContext.stream)
    {
        Printf(RED "ERROR: Can't open output file '%s'!\n", outputFile.c_str());

        return false;
    }

    if (useTextOutput)
    {
        string name = GetShaderName(blobName);
        outputContext.WriteTextPreamble(name.c_str(), "");
    }

    ShaderMake::WriteFileCallback writeFileCallback = useTextOutput
        ? &DataOutputContext::WriteDataAsTextCallback
        : &DataOutputContext::WriteDataAsBinaryCallback;

    // Write "blob" header
    if (!ShaderMake::WriteFileHeader(writeFileCallback, &outputContext))
    {
        Printf(RED "ERROR: Failed to write into output file '%s'!\n", outputFile.c_str());

        return false;
    }

    bool success = true;

    // Collect individual permutations
    for (const BlobEntry& entry : entries)
    {
        // Open compiled permutation file
        string file = entry.permutationFileWithoutExt + g_OutputExt;

        vector<uint8_t> fileData;
        if (ReadBinaryFile(file.c_str(), fileData))
        {
            if (!ShaderMake::WritePermutation(writeFileCallback, &outputContext, entry.combinedDefines, fileData.data(), fileData.size()))
            {
                Printf(RED "ERROR: Failed to write a shader permutation into '%s'!\n", outputFile.c_str());
                success = false;
            }
        }
        else
            success = false;

        if (!success)
            break;
    }

    if (useTextOutput)
        outputContext.WriteTextEpilog();

    return success;
}

void RemoveIntermediateBlobFiles(const vector<BlobEntry>& entries)
{
    for (const BlobEntry& entry : entries)
    {
        string file = entry.permutationFileWithoutExt + g_OutputExt;
        fs::remove(file);
    }
}

void SignalHandler(int32_t sig)
{
    UNUSED(sig);

    g_Terminate = true;

    Printf(RED "Aborting...\n");
}

int32_t main(int32_t argc, const char** argv)
{
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

    // Set envvar
    char envBuf[BUF_SIZE];
#ifdef _WIN32 // workaround for Windows
    snprintf(envBuf, sizeof(envBuf), "COMPILER=\"%s\"", g_Options.compiler);

    // Setup a directory where to look for the compiler first
    fs::path compilerPath = fs::path(g_Options.compiler).parent_path();
    SetDllDirectoryA(compilerPath.string().c_str());
#else
    snprintf(envBuf, sizeof(envBuf), "COMPILER=%s", g_Options.compiler);
#endif

    if (putenv(envBuf) != 0)
        return 1;

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

        for (uint32_t lineIndex = 0; getline(configStream, line); lineIndex++)
        {
            TrimConfigLine(line);

            // Skip an empty or commented line
            if (line.empty() || line[0] == '\n' || (line[0] == '/' && line[1] == '/'))
                continue;

            // TODO: preprocessor supports "#ifdef MACRO / #if 1 / #if 0", "#else" and "#endif"
            size_t pos = line.find("#ifdef");
            if (pos != string::npos)
            {
                pos += 6;
                pos += line.substr(pos).find_first_not_of(' ');

                string define = line.substr(pos);
                bool state = blocks.back() && find(g_Options.defines.begin(), g_Options.defines.end(), define) != g_Options.defines.end();

                blocks.push_back(state);
            }
            else if (line.find("#if 1") != string::npos)
                blocks.push_back(blocks.back());
            else if (line.find("#if 0") != string::npos)
                blocks.push_back(false);
            else if (line.find("#endif") != string::npos)
            {
                if (blocks.size() == 1)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#endif'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else
                    blocks.pop_back();
            }
            else if (line.find("#else") != string::npos)
            {
                if (blocks.size() < 2)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#else'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else if (blocks[blocks.size() - 2])
                    blocks.back() = !blocks.back();
            }
            else if (blocks.back())
            {
                if (!ExpandPermutations(lineIndex, line, configTime))
                    return 1;
            }
        }
    }

    // Process tasks
    if (!g_TaskData.empty())
    {
        Printf(WHITE "Compiling shaders using: %s\n", g_Options.compiler);

        g_OriginalTaskCount = (uint32_t)g_TaskData.size();
        g_ProcessedTaskCount = 0;
        g_FailedTaskCount = 0;

        // Retry limit for compilation task sub-process failures that can occur when threading
        g_TaskRetryCount = g_Options.retryCount;

        uint32_t threadsNum = max(g_Options.serial ? 1 : thread::hardware_concurrency(), 1u);

        vector<thread> threads(threadsNum);
        for (uint32_t i = 0; i < threadsNum; i++)
            threads[i] = thread(ExeCompile);

        for (uint32_t i = 0; i < threadsNum; i++)
            threads[i].join();

        // If a fatal error or a termination request happened, don't proceed to the blob building.
        if (g_Terminate)
            return 1;

        // Dump shader blobs
        for (const auto& [blobName, blobEntries] : g_ShaderBlobs)
        {
            // If a blob contains one entry with no defines, just skip it.
            // The individual file's output name is the same as the blob, and we're done here.
            if (blobEntries.size() == 1 && blobEntries[0].combinedDefines.empty())
                continue;

            // Validate that the blob doesn't contain any shaders with empty defines.
            // In such case, that individual shader's output file is the same as the blob output file, which wouldn't work.
            // We could detect this condition earlier and work around it by renaming the shader output file, if necessary.
            bool invalidEntry = false;
            for (const auto& entry : blobEntries)
            {
                if (entry.combinedDefines.empty())
                {
                    const string blobBaseName = fs::path(blobName).stem().generic_string();
                    Printf(RED "ERROR: Cannot create a blob for shader %s where some permutation(s) have no definitions!", blobBaseName.c_str());
                    invalidEntry = true;
                    break;
                }
            }

            if (invalidEntry)
            {
                if (g_Options.continueOnError)
                    continue;

                return 1;
            }

            if (g_Options.binaryBlob)
            {
                bool result = CreateBlob(blobName, blobEntries, false);
                if (!result && !g_Options.continueOnError)
                    return 1;
            }

            if (g_Options.headerBlob)
            {
                bool result = CreateBlob(blobName, blobEntries, true);
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

    return (g_Terminate || g_FailedTaskCount) ? 1 : 0;
}
