/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderMake {

struct ShaderConstant {
    const char* name;
    const char* value;
};

struct ShaderBlobEntry {
    uint32_t permutationSize;
    uint32_t dataSize;
};

typedef bool (*WriteFileCallback)(const void* data, size_t size, void* context);

// Metal converter bundle, the "--metalFromDXIL" output (little-endian): this header, "metal-shaderconverter" metallib (8-byte aligned)
// and reflection JSON (followed by a zero terminator, not included in "reflectionSize"). Offsets are from the bundle start
constexpr uint32_t MetalConverterBundleMagic = 0x424D4D53; // "SMMB"
constexpr uint32_t MetalConverterBundleVersion = 1;

struct MetalConverterBundleHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t metallibOffset;
    uint32_t metallibSize;
    uint32_t reflectionOffset;
    uint32_t reflectionSize;
};

void EnumeratePermutationsInBlob(const void* blob, size_t blobSize, std::vector<std::string>& permutations);
bool FindPermutationInBlob(const void* blob, size_t blobSize, const ShaderConstant* constants, uint32_t numConstants, const void** pBinary, size_t* pSize);
bool WriteFileHeader(WriteFileCallback write, void* context);
bool WritePermutation(WriteFileCallback write, void* context, const std::string& permutationKey, const void* binary, size_t binarySize);
std::string FormatShaderNotFoundMessage(const void* blob, size_t blobSize, const ShaderConstant* constants, uint32_t numConstants);
std::vector<size_t> GetSortedConstantsIndices(const std::vector<std::string>& constants);
bool ParseMetalConverterBundle(const void* bundle, size_t bundleSize, const void** pMetallib, size_t* pMetallibSize, const char** pReflection, size_t* pReflectionSize);

} // namespace ShaderMake
