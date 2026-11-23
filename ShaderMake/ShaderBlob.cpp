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

#include "ShaderBlob.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <sstream>

namespace ShaderMake {

static constexpr char g_BlobSignature[] = "NVSP";
static constexpr size_t g_BlobSignatureSize = sizeof(g_BlobSignature) - 1;

static inline bool ReadBlobEntry(const void*& blob, size_t& blobSize, ShaderBlobEntry& header, const char*& permutation) {
    if (blobSize <= sizeof(header))
        return false;

    memcpy(&header, blob, sizeof(header));
    size_t entrySize = sizeof(header) + header.dataSize + header.permutationSize;
    if (header.dataSize == 0 || blobSize < entrySize)
        return false;

    permutation = static_cast<const char*>(blob) + sizeof(header);
    blob = static_cast<const char*>(blob) + entrySize;
    blobSize -= entrySize;
    return true;
}

bool FindPermutationInBlob(const void* blob, size_t blobSize, const ShaderConstant* constants, uint32_t numConstants, const void** pBinary, size_t* pSize) {
    if (!blob || blobSize < g_BlobSignatureSize)
        return false;

    if (!pBinary || !pSize)
        return false;

    if (memcmp(blob, g_BlobSignature, g_BlobSignatureSize) != 0) {
        if (numConstants != 0)
            return false;

        // A raw shader binary is the default permutation
        *pBinary = blob;
        *pSize = blobSize;
        return true;
    }

    blob = static_cast<const char*>(blob) + g_BlobSignatureSize;
    blobSize -= g_BlobSignatureSize;

    // Match the writer's order of full "name=value" strings
    std::vector<std::string> constantDefines((size_t)numConstants);
    for (uint32_t n = 0; n < numConstants; n++) {
        const ShaderConstant& constant = constants[n];
        constantDefines[n] = std::string(constant.name) + "=" + constant.value;
    }
    std::sort(constantDefines.begin(), constantDefines.end());

    std::string permutation;
    for (uint32_t n = 0; n < numConstants; n++) {
        if (n != 0)
            permutation += ' ';
        permutation += constantDefines[n];
    }

    ShaderBlobEntry header;
    const char* entryPermutation;
    while (ReadBlobEntry(blob, blobSize, header, entryPermutation)) {
        if ((header.permutationSize == permutation.size()) && ((permutation.size() == 0) || (strncmp(entryPermutation, permutation.data(), permutation.size()) == 0))) {
            *pBinary = entryPermutation + header.permutationSize;
            *pSize = header.dataSize;

            return true;
        }
    }

    return false;
}

void EnumeratePermutationsInBlob(const void* blob, size_t blobSize, std::vector<std::string>& permutations) {
    if (!blob || blobSize < g_BlobSignatureSize)
        return;

    if (memcmp(blob, g_BlobSignature, g_BlobSignatureSize) != 0)
        return;

    blob = static_cast<const char*>(blob) + g_BlobSignatureSize;
    blobSize -= g_BlobSignatureSize;

    ShaderBlobEntry header;
    const char* permutation;
    while (ReadBlobEntry(blob, blobSize, header, permutation)) {
        if (header.permutationSize > 0)
            permutations.emplace_back(permutation, header.permutationSize);
        else
            permutations.emplace_back("<default>");
    }
}

std::string FormatShaderNotFoundMessage(const void* blob, size_t blobSize, const ShaderConstant* constants, uint32_t numConstants) {
    std::stringstream ss;
    ss << "Couldn't find the required shader permutation in the blob, or the blob is corrupted." << std::endl;
    ss << "Required permutation key: " << std::endl;

    if (numConstants) {
        for (uint32_t n = 0; n < numConstants; n++) {
            const ShaderConstant& constant = constants[n];
            ss << constant.name << '=' << constant.value << ';';
        }
    } else
        ss << "<default>";

    ss << std::endl;

    std::vector<std::string> permutations;
    EnumeratePermutationsInBlob(blob, blobSize, permutations);

    if (!permutations.empty()) {
        ss << "Permutations available in the blob:" << std::endl;
        for (const std::string& key : permutations)
            ss << key << std::endl;
    } else
        ss << "No permutations found in the blob.";

    return ss.str();
}

bool WriteFileHeader(WriteFileCallback write, void* context) {
    return write(g_BlobSignature, g_BlobSignatureSize, context);
}

bool WritePermutation(WriteFileCallback write, void* context, const std::string& permutationKey, const void* binary, size_t binarySize) {
    ShaderBlobEntry binaryEntry{};
    binaryEntry.permutationSize = (uint32_t)permutationKey.size();
    binaryEntry.dataSize = (uint32_t)binarySize;

    bool success = write(&binaryEntry, sizeof(binaryEntry), context);
    success &= write(permutationKey.data(), binaryEntry.permutationSize, context);
    success &= write(binary, binarySize, context);
    return success;
}

std::vector<size_t> GetSortedConstantsIndices(const std::vector<std::string>& constants) {
    std::vector<size_t> sortedDefinesIndices(constants.size());
    std::iota(sortedDefinesIndices.begin(), sortedDefinesIndices.end(), 0);

    // Preserve input order for equal constants
    std::stable_sort(sortedDefinesIndices.begin(), sortedDefinesIndices.end(),
        [&constants](size_t i1, size_t i2) { return constants[i1] < constants[i2]; });

    return sortedDefinesIndices;
}

bool ParseMetalConverterBundle(const void* bundle, size_t bundleSize, const void** pMetallib, size_t* pMetallibSize, const char** pReflection, size_t* pReflectionSize) {
    if (!bundle || bundleSize < sizeof(MetalConverterBundleHeader))
        return false;

    if (!pMetallib || !pMetallibSize || !pReflection || !pReflectionSize)
        return false;

    MetalConverterBundleHeader header;
    memcpy(&header, bundle, sizeof(header));

    if (header.magic != MetalConverterBundleMagic || header.version != MetalConverterBundleVersion)
        return false;

    // Layout: header, metallib, reflection (with a zero terminator), non-overlapping and in this order
    const uint8_t* data = static_cast<const uint8_t*>(bundle);
    uint64_t metallibEnd = (uint64_t)header.metallibOffset + header.metallibSize;
    uint64_t reflectionEnd = (uint64_t)header.reflectionOffset + header.reflectionSize;
    if (header.metallibOffset < sizeof(header) || header.metallibOffset % 8 != 0 || header.metallibSize == 0 || header.reflectionOffset < metallibEnd)
        return false;
    if (metallibEnd > bundleSize || reflectionEnd >= bundleSize || data[reflectionEnd] != 0)
        return false;

    *pMetallib = data + header.metallibOffset;
    *pMetallibSize = header.metallibSize;
    *pReflection = reinterpret_cast<const char*>(data + header.reflectionOffset);
    *pReflectionSize = header.reflectionSize;

    return true;
}

} // namespace ShaderMake
