/* bgt_legacy.h - header for the bit-exact BGT formats
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2026 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#pragma once

#include <string>
#include <angelscript.h>

class CScriptDictionary;

std::string bgt_string_compress(const std::string& data);
std::string bgt_string_decompress(const std::string& blob);
std::string bgt_serialize(CScriptDictionary* dict);
CScriptDictionary* bgt_deserialize(const std::string& blob);
std::string bgt_string_encrypt(const std::string& data, const std::string& password);
std::string bgt_string_decrypt(const std::string& blob, const std::string& password);
std::string bgt_swcr_encrypt(const std::string& data, const std::string& password);
std::string bgt_swcr_decrypt(const std::string& blob, const std::string& password);
std::string bgt_generate_computer_id(const std::string& application_id, bool hardware_only);
// Whether a blob is a BGT-serialized dictionary rather than one of NVGT's own. Tight
// enough that NVGT's headers cannot match: both begin 0x0E, which is not base64.
bool bgt_looks_serialized(const std::string& blob);

void RegisterBgtLegacyFunctions(asIScriptEngine* engine);
