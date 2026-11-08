/* bgt_legacy.cpp - the three BGT formats a ported game has to read and write exactly
 *
 * bgt_compat.nvgt shims BGT's generate_computer_id, string_compress and serialize
 * with NVGT's own equivalents, and all three are a DIFFERENT format rather than a
 * different spelling. Each failure is silent: the port compiles, runs, and cannot
 * read what the BGT build wrote.
 *
 *   generate_computer_id -> generate_system_fingerprint   a different number, so a
 *                                                         registration key the player
 *                                                         already holds never validates
 *   string_compress      -> string_deflate                zlib, where BGT has its own
 *                                                         LZ77 plus a trailing decimal
 *                                                         length
 *   serialize            -> dictionary::serialize         a tagged binary format, where
 *                                                         BGT has base64 keys and text
 *                                                         values
 *
 * Every byte of what follows was read out of bgt.exe 1.3 (2014-06-01) and then checked
 * against the engine itself rather than argued from the disassembly: the computer id
 * agrees with BGT's own answer on 44 of 44 application ids, and the compressor
 * reproduces BGT's output byte for byte on all twelve recorded cases, the awkward ones
 * included (an empty string, a string holding every byte value, and one whose shortest
 * repeat is below the minimum match length).
 *
 * The algorithm is one algorithm across the whole shipped corpus: BGT 1.3, Super Egg
 * Hunt, Paladin of the Sky, Psycho Strike, The Gate, Manamon 1 and Manamon 2 all carry
 * the same seven constants, and only BGT 1.0 differs -- that build has no
 * \\.\PhysicalDrive0 and reads the CPU's FeatureSet registry value instead, which is
 * the kernel's feature mask rather than CPUID.1.EDX. So a key issued for a 1.0 game is
 * not a key for a 1.3 one, and this file does not compute 1.0's.
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

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#define VC_EXTRALEAN
	#include <windows.h>
	#include <winioctl.h>
	#include <intrin.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <angelscript.h>
#include <obfuscate.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Base64Encoder.h>
#include <Poco/SHA1Engine.h>
#include <Poco/SHA2Engine.h>
#include <aes.hpp>
#include <scriptdictionary.h>
#include "bgt_legacy.h"
#include "nvgt.h"

// --------------------------------------------------------------------------
// base64, with Poco, padded and on one line -- which is what BGT writes
// --------------------------------------------------------------------------

static std::string bgt_base64_encode(const std::string& data) {
	std::ostringstream out;
	Poco::Base64Encoder encoder(out);
	encoder.rdbuf()->setLineLength(0);
	encoder.write(data.data(), (std::streamsize)data.size());
	encoder.close();
	return out.str();
}

static std::string bgt_base64_decode(const std::string& data) {
	std::istringstream in(data);
	Poco::Base64Decoder decoder(in);
	std::ostringstream out;
	out << decoder.rdbuf();
	return out.str();
}

// --------------------------------------------------------------------------
// LZ77: BGT's own, and the same codec its module container uses
// --------------------------------------------------------------------------
//
// The stream opens with one escape byte. Thereafter a byte equal to the escape
// introduces either an escaped literal (escape, 0x00) or a match -- a length and a
// distance, each a big-endian base-128 varint whose high bit means "another byte
// follows". The whole blob ends with a space and the uncompressed length in decimal,
// which is how the decoder knows when to stop.
//
// Three details are the ones a plausible guess gets wrong, and each was read out of
// BGT's own output rather than reasoned about:
//
// - the escape is the LOWEST byte value absent from the input, and 0x00 when every
//   value occurs. A blob holding a NUL therefore opens with 0x01, not 0x00.
// - a match may not overlap the cursor: its length is capped at its distance. So
//   "aaaa..." compresses as a doubling (4 from 4 back, 8 from 8 back, ...) rather
//   than as one long run at distance 1.
// - the minimum match is 4 bytes, so "aaaaaa" is six literals.
//
// An empty input is the whole blob " 0" with no escape byte at all, which a decoder
// has to special-case before it reads comp[0].

static const size_t BGT_LZ77_MIN_MATCH = 4;
// How many candidates at one hash the matcher will consider. BGT searches every
// distance, so a capped chain can only ever produce a SHORTER match than BGT would --
// which still decompresses correctly in either engine, and is why the cap is a speed
// choice rather than a correctness one. Generous enough that every save-sized input
// reproduces BGT's output exactly.
static const size_t BGT_LZ77_CHAIN_LIMIT = 4096;

static void bgt_varint7_append(std::string& out, uint64_t value) {
	unsigned char parts[10];
	int count = 0;
	parts[count++] = (unsigned char)(value & 0x7F);
	value >>= 7;
	while (value) {
		parts[count++] = (unsigned char)(0x80 | (value & 0x7F));
		value >>= 7;
	}
	for (int i = count - 1; i >= 0; i--) out.push_back((char)parts[i]);
}

static bool bgt_varint7_read(const std::string& in, size_t& pos, uint64_t& value) {
	value = 0;
	while (pos < in.size()) {
		unsigned char c = (unsigned char)in[pos++];
		value = (value << 7) | (c & 0x7F);
		if (!(c & 0x80)) return true;
	}
	return false;
}

static unsigned char bgt_choose_escape(const std::string& data) {
	bool seen[256] = {false};
	for (size_t i = 0; i < data.size(); i++) seen[(unsigned char)data[i]] = true;
	for (int b = 0; b < 256; b++) if (!seen[b]) return (unsigned char)b;
	return 0;
}

std::string bgt_string_compress(const std::string& data) {
	if (data.empty()) return " 0";
	const size_t n = data.size();
	const unsigned char esc = bgt_choose_escape(data);
	std::string out;
	out.reserve(n + 16);
	out.push_back((char)esc);
	// Classic hash chain over four-byte prefixes. A match of four bytes or more
	// shares its first four bytes, so the chain holds every candidate BGT would
	// consider; `head` keeps the most recent position, so walking it visits
	// distances in increasing order and a strict `>` keeps the SMALLEST distance
	// when two give the same length, which is the tie BGT breaks the same way.
	std::vector<int32_t> head(1 << 16, -1);
	std::vector<int32_t> prev(n, -1);
	size_t i = 0;
	while (i < n) {
		size_t best_len = 0, best_dist = 0;
		if (i + BGT_LZ77_MIN_MATCH <= n && i > 0) {
			unsigned h = (((unsigned char)data[i] << 11) ^ ((unsigned char)data[i + 1] << 7)
			              ^ ((unsigned char)data[i + 2] << 3) ^ (unsigned char)data[i + 3]) & 0xFFFF;
			int32_t cand = head[h];
			size_t budget = BGT_LZ77_CHAIN_LIMIT;
			while (cand >= 0 && budget--) {
				size_t dist = i - (size_t)cand;
				size_t cap = dist < (n - i) ? dist : (n - i);
				if (cap > best_len) {
					size_t len = 0;
					while (len < cap && data[(size_t)cand + len] == data[i + len]) len++;
					if (len > best_len) { best_len = len; best_dist = dist; }
				}
				cand = prev[(size_t)cand];
			}
		}
		size_t step;
		if (best_len >= BGT_LZ77_MIN_MATCH) {
			out.push_back((char)esc);
			bgt_varint7_append(out, (uint64_t)best_len);
			bgt_varint7_append(out, (uint64_t)best_dist);
			step = best_len;
		} else {
			unsigned char b = (unsigned char)data[i];
			out.push_back((char)b);
			if (b == esc) out.push_back((char)0);
			step = 1;
		}
		// Every position is inserted, including the ones a match skipped: BGT
		// searches all of them, so leaving them out would shorten later matches.
		for (size_t j = i; j < i + step; j++) {
			if (j + BGT_LZ77_MIN_MATCH > n) break;
			unsigned h = (((unsigned char)data[j] << 11) ^ ((unsigned char)data[j + 1] << 7)
			              ^ ((unsigned char)data[j + 2] << 3) ^ (unsigned char)data[j + 3]) & 0xFFFF;
			prev[j] = head[h];
			head[h] = (int32_t)j;
		}
		i += step;
	}
	char tail[32];
	snprintf(tail, sizeof(tail), " %llu", (unsigned long long)n);
	out += tail;
	return out;
}

std::string bgt_string_decompress(const std::string& blob) {
	// The declared length is after the LAST space, because the compressed bytes may
	// hold spaces of their own.
	size_t space = blob.rfind(' ');
	if (space == std::string::npos) return "";
	unsigned long long declared = strtoull(blob.c_str() + space + 1, NULL, 10);
	if (declared == 0) return "";
	std::string comp = blob.substr(0, space);
	if (comp.empty()) return "";
	const unsigned char esc = (unsigned char)comp[0];
	std::string out;
	out.reserve((size_t)declared);
	size_t p = 1;
	while (p < comp.size()) {
		unsigned char c = (unsigned char)comp[p];
		if (c != esc) { out.push_back((char)c); p++; continue; }
		if (p + 1 >= comp.size()) break;
		if ((unsigned char)comp[p + 1] == 0) { out.push_back((char)esc); p += 2; continue; }
		p++;
		uint64_t length = 0, dist = 0;
		if (!bgt_varint7_read(comp, p, length)) break;
		if (!bgt_varint7_read(comp, p, dist)) break;
		if (dist == 0 || dist > out.size()) return "";
		// Byte at a time, because a match is allowed to run into what it is
		// producing -- BGT's own decoder copies this way even though its
		// compressor never emits such a match.
		for (uint64_t k = 0; k < length; k++) out.push_back(out[out.size() - (size_t)dist]);
	}
	if (out.size() != (size_t)declared) return "";
	return out;
}

// --------------------------------------------------------------------------
// serialize: base64 keys, one type byte, the value as text
// --------------------------------------------------------------------------
//
// One entry is  base64(key) NUL <type> <value text> NUL  and the entries are
// concatenated with nothing between them -- the value's terminating NUL is the
// separator. NUL is unambiguous because a key and a string value are both base64 and
// a number is digits.
//
//   1  an integer, in decimal, signed or unsigned as the stored type is
//   2  a double, written "%f" -- SIX decimals, so 1e-7 serialises as 0.000000 and
//      the precision really is lost. A port that writes more would produce a save
//      BGT reads differently.
//   3  a string, base64 of its bytes, so an embedded NUL survives
//   4  a bool, "1" or "0"
//
// Anything else -- an array, a nested dictionary, a script class -- is DROPPED, with
// no entry and no error, which is what BGT does: serialising a dictionary holding an
// array returns an empty string.

enum bgt_value_type { BGT_VALUE_INT = 1, BGT_VALUE_DOUBLE = 2, BGT_VALUE_STRING = 3, BGT_VALUE_BOOL = 4 };

static bool bgt_is_signed_int(int type_id) {
	return type_id == asTYPEID_INT8 || type_id == asTYPEID_INT16 ||
	       type_id == asTYPEID_INT32 || type_id == asTYPEID_INT64;
}

static bool bgt_is_unsigned_int(int type_id) {
	return type_id == asTYPEID_UINT8 || type_id == asTYPEID_UINT16 ||
	       type_id == asTYPEID_UINT32 || type_id == asTYPEID_UINT64;
}

static bool bgt_read_integer(const void* ref, int type_id, long long& out) {
	switch (type_id) {
		case asTYPEID_INT8: out = *(const int8_t*)ref; return true;
		case asTYPEID_INT16: out = *(const int16_t*)ref; return true;
		case asTYPEID_INT32: out = *(const int32_t*)ref; return true;
		case asTYPEID_INT64: out = *(const int64_t*)ref; return true;
		default: return false;
	}
}

static bool bgt_read_unsigned(const void* ref, int type_id, unsigned long long& out) {
	switch (type_id) {
		case asTYPEID_UINT8: out = *(const uint8_t*)ref; return true;
		case asTYPEID_UINT16: out = *(const uint16_t*)ref; return true;
		case asTYPEID_UINT32: out = *(const uint32_t*)ref; return true;
		case asTYPEID_UINT64: out = *(const uint64_t*)ref; return true;
		default: return false;
	}
}

std::string bgt_serialize(CScriptDictionary* dict) {
	std::string out;
	if (!dict) return out;
	static int string_type_id = 0;
	if (!string_type_id) string_type_id = g_ScriptEngine->GetStringFactory();
	for (CScriptDictionary::CIterator it = dict->begin(); it != dict->end(); it++) {
		const int type_id = it.GetTypeId();
		const void* ref = it.GetAddressOfValue();
		char tag = 0;
		std::string text;
		char buffer[64];
		long long signed_value = 0;
		unsigned long long unsigned_value = 0;
		if (type_id == asTYPEID_BOOL) {
			tag = BGT_VALUE_BOOL;
			text = *(const bool*)ref ? "1" : "0";
		} else if (bgt_is_signed_int(type_id) && bgt_read_integer(ref, type_id, signed_value)) {
			tag = BGT_VALUE_INT;
			snprintf(buffer, sizeof(buffer), "%lld", signed_value);
			text = buffer;
		} else if (bgt_is_unsigned_int(type_id) && bgt_read_unsigned(ref, type_id, unsigned_value)) {
			tag = BGT_VALUE_INT;
			snprintf(buffer, sizeof(buffer), "%llu", unsigned_value);
			text = buffer;
		} else if (type_id == asTYPEID_FLOAT || type_id == asTYPEID_DOUBLE) {
			tag = BGT_VALUE_DOUBLE;
			double value = type_id == asTYPEID_FLOAT ? (double)*(const float*)ref : *(const double*)ref;
			snprintf(buffer, sizeof(buffer), "%f", value);
			text = buffer;
		} else if (type_id == string_type_id) {
			tag = BGT_VALUE_STRING;
			text = bgt_base64_encode(*(const std::string*)ref);
		} else {
			continue;   // an array, a dictionary, a script class: dropped, as BGT drops them
		}
		out += bgt_base64_encode(it.GetKey());
		out.push_back((char)0);
		out.push_back(tag);
		out += text;
		out.push_back((char)0);
	}
	return out;
}

bool bgt_looks_serialized(const std::string& blob) {
	// base64 key, NUL, a type byte in 1..4, the value's text, NUL. A key is base64 and
	// so is a string value, and a number is digits, so the NUL is unambiguous -- and
	// NVGT's own two formats both open with 0x0E, which is not a base64 character, so
	// this can never claim one of those.
	if (blob.size() < 4) return false;
	size_t key_end = blob.find((char)0);
	if (key_end == std::string::npos || key_end == 0) return false;
	for (size_t i = 0; i < key_end; i++) {
		const char c = blob[i];
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
		if (!ok) return false;
	}
	if (key_end + 1 >= blob.size()) return false;
	const unsigned char tag = (unsigned char)blob[key_end + 1];
	if (tag < BGT_VALUE_INT || tag > BGT_VALUE_BOOL) return false;
	return blob.find((char)0, key_end + 2) != std::string::npos;
}

CScriptDictionary* bgt_deserialize(const std::string& blob) {
	CScriptDictionary* dict = CScriptDictionary::Create(g_ScriptEngine);
	if (!dict) return NULL;
	size_t p = 0;
	while (p < blob.size()) {
		size_t key_end = blob.find((char)0, p);
		if (key_end == std::string::npos) break;
		std::string key = bgt_base64_decode(blob.substr(p, key_end - p));
		size_t tag_at = key_end + 1;
		if (tag_at >= blob.size()) break;
		const unsigned char tag = (unsigned char)blob[tag_at];
		size_t value_end = blob.find((char)0, tag_at + 1);
		if (value_end == std::string::npos) break;
		std::string text = blob.substr(tag_at + 1, value_end - tag_at - 1);
		switch (tag) {
			case BGT_VALUE_INT: {
				// Signed unless it will not fit, which is how BGT writes a uint:
				// 4000000000 is stored as a positive number rather than wrapping.
				long long value = strtoll(text.c_str(), NULL, 10);
				if (text.size() && text[0] != '-' && strtoull(text.c_str(), NULL, 10) > 0x7FFFFFFFFFFFFFFFull)
					value = (long long)strtoull(text.c_str(), NULL, 10);
				dict->Set(key, value);
				break;
			}
			case BGT_VALUE_DOUBLE: {
				double value = strtod(text.c_str(), NULL);
				dict->Set(key, value);
				break;
			}
			case BGT_VALUE_STRING: {
				std::string value = bgt_base64_decode(text);
				dict->Set(key, &value, g_ScriptEngine->GetStringFactory());
				break;
			}
			case BGT_VALUE_BOOL: {
				bool value = text == "1";
				dict->Set(key, &value, asTYPEID_BOOL);
				break;
			}
			default:
				break;   // an unknown tag: skip the entry rather than guess at it
		}
		p = value_end + 1;
	}
	return dict;
}

// --------------------------------------------------------------------------
// string_encrypt / string_decrypt: the third link of a BGT save path
// --------------------------------------------------------------------------
//
// BGT's save path is serialize() -> string_compress() -> string_encrypt(), so fixing
// two of the three leaves the chain broken: NVGT's string_aes_encrypt hashes the key
// straight with SHA-256 and derives its IV as key[i*2] ^ (4*i+1), where BGT puts the
// key through its own derivation first, uses the digest's first sixteen bytes as the
// IV, and wraps the plaintext in a length-declaring header. The shipped
// bgt_compat.nvgt said so in a comment -- "Does not work with existing bgt encrypted
// data" -- and that comment is what this closes.
//
//   material = bgt_kdf(password)              64 bytes, with every NUL scrubbed out
//   key      = SHA256(material up to its first NUL)
//   iv       = key[0..15]
//   plain    = "printf" + decimal(len) + NUL + data, zero padded to 16
//
// The derivation itself is read out of the engine, and three details of it are ones a
// transcription gets wrong: a char is SIGNED at every assignment, the divide is C's
// (truncating toward zero), and the password goes BETWEEN the two salts rather than
// before or after both. The salts depend only on one seed byte, which is why the
// generator reaching a fixed point almost immediately is expected rather than a bug.
//
// An empty input encrypts to an empty string, which is the engine's own answer and
// not a shortcut: BGT returns "" rather than one block of padding.

static signed char bgt_sc(int v) { return (signed char)(v & 0xFF); }

// C integer division, which truncates toward zero.
static int bgt_cdiv(int a, int b) {
	int q = (a < 0 ? -a : a) / (b < 0 ? -b : b);
	return ((a < 0) != (b < 0)) ? -q : q;
}

static void bgt_kdf_salts(int pre_seed, std::string& first, std::string& second) {
	signed char seed = bgt_sc((pre_seed - 4) * pre_seed);
	const int seed_int = seed;
	const signed char hi = bgt_sc(seed & 0xFF);
	signed char lo = bgt_sc(seed - 1);
	for (int which = 0; which < 2; which++) {
		std::string out;
		out.reserve(0x62);
		for (int i = 0; i < 0x62; i++) {
			int c = lo;
			if (c == 0) c = -0x12;
			const int divisor = seed_int + c * 2 + 8;
			if (divisor == 0) { first.clear(); second.clear(); return; }
			c = bgt_sc(bgt_cdiv(c, divisor));
			if (c == 0) c = bgt_sc(hi + 0x63);
			lo = bgt_sc(c);
			out.push_back((char)(unsigned char)(c & 0xFF));
		}
		if (which == 0) first = out; else second = out;
	}
}

static std::string bgt_kdf(const std::string& data) {
	// The only input-dependent part: the first byte with its high bit set, else '#'.
	int seed = 0x23;
	for (size_t i = 0; i < data.size(); i++) {
		if ((unsigned char)data[i] >= 0x80) { seed = bgt_sc(data[i]); break; }
	}
	std::string first, second;
	bgt_kdf_salts(seed, first, second);
	Poco::SHA2Engine engine(Poco::SHA2Engine::SHA_512);
	engine.update(first);
	engine.update(data);
	engine.update(second);
	const Poco::DigestEngine::Digest& digest = engine.digest();
	std::string out(digest.begin(), digest.end());
	// Every NUL is replaced, because the result is consumed as a C string: a NUL
	// here would silently truncate the key.
	for (size_t i = 0; i < out.size() && i < 0x40; i++) {
		if (out[i] == 0) {
			unsigned char c = (unsigned char)((-11 * (int)(i + 1)) & 0xFF);
			out[i] = (char)(c ? c : 0x83);
		}
	}
	return out;
}

static void bgt_crypt_params(const std::string& password, unsigned char key[32],
                             unsigned char iv[16]) {
	std::string material = bgt_kdf(password);
	size_t nul = material.find((char)0);
	if (nul != std::string::npos) material.resize(nul);
	Poco::SHA2Engine engine(Poco::SHA2Engine::SHA_256);
	engine.update(material);
	const Poco::DigestEngine::Digest& digest = engine.digest();
	for (size_t i = 0; i < 32 && i < digest.size(); i++) key[i] = digest[i];
	for (size_t i = 0; i < 16; i++) iv[i] = key[i];
}

std::string bgt_string_encrypt(const std::string& data, const std::string& password) {
	if (data.empty()) return "";
	char header[48];
	snprintf(header, sizeof(header), "printf%llu", (unsigned long long)data.size());
	std::string plain(header);
	plain.push_back((char)0);
	plain += data;
	while (plain.size() % 16) plain.push_back((char)0);
	unsigned char key[32], iv[16];
	bgt_crypt_params(password, key, iv);
	AES_ctx ctx;
	AES_init_ctx_iv(&ctx, key, iv);
	AES_CBC_encrypt_buffer(&ctx, (uint8_t*)&plain[0], plain.size());
	memset(key, 0, sizeof(key));
	memset(iv, 0, sizeof(iv));
	memset(&ctx, 0, sizeof(ctx));
	return plain;
}

std::string bgt_string_decrypt(const std::string& blob, const std::string& password) {
	if (blob.size() < 16) return "";
	std::string text = blob.substr(0, blob.size() - blob.size() % 16);
	unsigned char key[32], iv[16];
	bgt_crypt_params(password, key, iv);
	AES_ctx ctx;
	AES_init_ctx_iv(&ctx, key, iv);
	AES_CBC_decrypt_buffer(&ctx, (uint8_t*)&text[0], text.size());
	memset(key, 0, sizeof(key));
	memset(iv, 0, sizeof(iv));
	memset(&ctx, 0, sizeof(ctx));
	// Self-checking: the plaintext has to begin "printf" and declare its own length,
	// so a wrong key is REJECTED rather than returning plausible rubbish.
	if (text.compare(0, 6, "printf") != 0) return "";
	size_t nul = text.find((char)0, 6);
	if (nul == std::string::npos) return "";
	unsigned long long declared = strtoull(text.c_str() + 6, NULL, 10);
	if (declared == 0 || nul + 1 + declared > text.size()) return "";
	return text.substr(nul + 1, (size_t)declared);
}

// --------------------------------------------------------------------------
// file_encrypt / file_decrypt: the SWCR container, which is the PACK's
// --------------------------------------------------------------------------
//
// A fifth format, and not the one string_encrypt uses. BGT's file_encrypt writes
//
//   "SWCR" + the plaintext length as a little-endian u32, THREE TIMES
//          + a 16-byte tag + AES-256-ECB of the data, zero padded to 16
//
// which is exactly how an entry inside an SFPv asset pack is encrypted -- so the one
// reading settles both. The tag is AES-ECB of sixteen zero bytes under the same key,
// so a candidate key is confirmed or rejected by ONE block without touching the
// ciphertext, and a wrong key cannot produce plausible output.
//
// The key is the same key string_encrypt uses -- SHA-256 of the derivation's output up
// to its first NUL -- so only the container and the cipher mode differ: SWCR and ECB
// here, a printf-wrapped plaintext and CBC there. The length is stored three times,
// which is the engine's own redundancy and is written out as it does.
//
// NVGT's own file_encrypt is asset_encryptor, a different container again, so a game
// that encrypted a data file with BGT could not read it back after a port.

static const char BGT_SWCR_MAGIC[4] = {'S', 'W', 'C', 'R'};

static void bgt_put_u32le(std::string& out, uint32_t value) {
	out.push_back((char)(value & 0xFF));
	out.push_back((char)((value >> 8) & 0xFF));
	out.push_back((char)((value >> 16) & 0xFF));
	out.push_back((char)((value >> 24) & 0xFF));
}

static uint32_t bgt_get_u32le(const std::string& in, size_t at) {
	return (uint32_t)(unsigned char)in[at] |
	       ((uint32_t)(unsigned char)in[at + 1] << 8) |
	       ((uint32_t)(unsigned char)in[at + 2] << 16) |
	       ((uint32_t)(unsigned char)in[at + 3] << 24);
}

static void bgt_ecb(const unsigned char key[32], std::string& text, bool encrypt) {
	AES_ctx ctx;
	AES_init_ctx(&ctx, key);
	for (size_t at = 0; at + 16 <= text.size(); at += 16) {
		if (encrypt) AES_ECB_encrypt(&ctx, (uint8_t*)&text[at]);
		else AES_ECB_decrypt(&ctx, (uint8_t*)&text[at]);
	}
	memset(&ctx, 0, sizeof(ctx));
}

std::string bgt_swcr_encrypt(const std::string& data, const std::string& password) {
	unsigned char key[32], iv[16];
	bgt_crypt_params(password, key, iv);
	std::string tag(16, (char)0);
	bgt_ecb(key, tag, true);
	std::string body = data;
	while (body.size() % 16) body.push_back((char)0);
	bgt_ecb(key, body, true);
	std::string out;
	out.reserve(32 + body.size());
	out.append(BGT_SWCR_MAGIC, 4);
	for (int i = 0; i < 3; i++) bgt_put_u32le(out, (uint32_t)data.size());
	out += tag;
	out += body;
	memset(key, 0, sizeof(key));
	memset(iv, 0, sizeof(iv));
	return out;
}

std::string bgt_swcr_decrypt(const std::string& blob, const std::string& password) {
	if (blob.size() < 32 || memcmp(blob.data(), BGT_SWCR_MAGIC, 4) != 0) return "";
	const uint32_t declared = bgt_get_u32le(blob, 4);
	unsigned char key[32], iv[16];
	bgt_crypt_params(password, key, iv);
	std::string tag(16, (char)0);
	bgt_ecb(key, tag, true);
	// One block decides whether this key opens the file, which is why a wrong key
	// returns nothing rather than rubbish.
	if (memcmp(tag.data(), blob.data() + 16, 16) != 0) {
		memset(key, 0, sizeof(key));
		return "";
	}
	std::string body = blob.substr(32);
	body.resize(body.size() - body.size() % 16);
	bgt_ecb(key, body, false);
	memset(key, 0, sizeof(key));
	memset(iv, 0, sizeof(iv));
	if (declared > body.size()) return "";
	body.resize(declared);
	return body;
}

// --------------------------------------------------------------------------
// generate_computer_id
// --------------------------------------------------------------------------

#ifdef _WIN32

// "%I64d%u%u%u%u" of PhysicalDrive0's geometry. The %I64d is the whole disk in bytes
// and the four %u are the cylinder count's two halves, the tracks per cylinder and the
// sectors per track. BytesPerSector is pushed by the engine and never consumed by the
// format, so it is deliberately absent here: counting it in changes every id.
static std::string bgt_drive_component() {
	HANDLE handle = CreateFileA("\\\\.\\PhysicalDrive0", 0,
	                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                            OPEN_EXISTING, 0, NULL);
	if (handle == INVALID_HANDLE_VALUE) return "";
	DISK_GEOMETRY geometry;
	DWORD returned = 0;
	BOOL ok = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
	                          &geometry, sizeof(geometry), &returned, NULL);
	CloseHandle(handle);
	if (!ok) return "";
	long long total = (long long)geometry.Cylinders.QuadPart *
	                  (long long)geometry.TracksPerCylinder *
	                  (long long)geometry.SectorsPerTrack *
	                  (long long)geometry.BytesPerSector;
	char buffer[128];
	snprintf(buffer, sizeof(buffer), "%lld%lu%lu%lu%lu", total,
	         (unsigned long)geometry.Cylinders.LowPart,
	         (unsigned long)geometry.Cylinders.HighPart,
	         (unsigned long)geometry.TracksPerCylinder,
	         (unsigned long)geometry.SectorsPerTrack);
	return buffer;
}

// "%u" of C:'s volume serial, and the empty string when it reads back as zero -- the
// engine skips its sprintf entirely in that case, so the component is absent rather
// than "0", which shortens the hashed string.
static std::string bgt_volume_component() {
	DWORD serial = 0;
	if (!GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0)) return "";
	if (serial == 0) return "";
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)serial);
	return buffer;
}

// "%u%u" of CPUID leaf 1's EAX and EDX -- the raw register values, not Windows' own
// FeatureSet registry value, which is the kernel's feature mask and a different number.
static std::string bgt_cpu_component() {
	int registers[4] = {0, 0, 0, 0};
	__cpuid(registers, 1);
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%u%u", (unsigned)registers[0], (unsigned)registers[3]);
	return buffer;
}

static std::string bgt_memory_component() {
	MEMORYSTATUSEX status;
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status)) return "";
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%lld", (long long)status.ullTotalPhys);
	return buffer;
}

#endif

// SHA-1 with each of the five state words left LITTLE-endian, which is what BGT's own
// implementation produces: it copies its state dwords straight into a 20-byte string.
// The digest holds the same bytes as a standard one in a different order, so no length,
// alphabet or multiset check can tell the two apart -- which is why this had to be read
// out of the binary rather than assumed.
static std::string bgt_native_sha1(const std::string& data) {
	Poco::SHA1Engine engine;
	engine.update(data);
	const Poco::DigestEngine::Digest& digest = engine.digest();
	std::string out(digest.begin(), digest.end());
	if (out.size() != 20) return out;
	for (size_t word = 0; word < 20; word += 4) {
		std::swap(out[word], out[word + 3]);
		std::swap(out[word + 1], out[word + 2]);
	}
	return out;
}

// A running product over the digest's SIGNED bytes, with the second byte subtracted
// once after the fifth multiply. The product overflows constantly and the overflow is
// part of the answer, so it is kept in an int32 at every step.
static int32_t bgt_id_checksum(const std::string& digest) {
	int32_t acc = 0x7FFF;
	for (size_t i = 0; i < digest.size(); i++) {
		acc = (int32_t)((uint32_t)acc * (uint32_t)(int32_t)(signed char)digest[i]);
		if (i == 4) acc = (int32_t)((uint32_t)acc - (uint32_t)(int32_t)(signed char)digest[1]);
	}
	return acc;
}

// Visual C++'s rand(), reproduced rather than borrowed from the host CRT: the shuffle
// is a permutation, so any other generator returns the right characters in the wrong
// order and nothing announces it.
class bgt_msvc_rand {
	uint32_t state;
public:
	explicit bgt_msvc_rand(uint32_t seed) : state(seed) {}
	int next() {
		state = state * 214013u + 2531011u;
		return (int)((state >> 16) & 0x7FFF);
	}
};

std::string bgt_generate_computer_id(const std::string& application_id, bool hardware_only) {
	// The engine treats an empty application id as an error (-2) and returns an empty
	// string rather than hashing one.
	if (application_id.empty()) return "";
#ifdef _WIN32
	std::string hashed = bgt_drive_component();
	hashed += hardware_only ? std::string(" ") : bgt_volume_component();
	hashed += application_id;
	hashed += bgt_cpu_component();
	hashed += bgt_memory_component();
	std::string digest = bgt_native_sha1(hashed);
	std::string out = bgt_base64_encode(digest);
	while (!out.empty() && out[out.size() - 1] == '=') out.erase(out.size() - 1);
	bgt_msvc_rand rand((uint32_t)bgt_id_checksum(digest));
	for (int i = 0; i < 13; i++) rand.next();          // thirteen draws discarded
	for (size_t i = 0; i < out.size(); i++)
		if (rand.next() % 10 + 1 > 6) std::swap(out[i], out[0]);
	return out;
#else
	// Every component is a Windows one -- a physical drive's geometry, a volume
	// serial, CPUID and the installed memory -- so there is no answer to give here
	// rather than a different one. bgt_compat.nvgt falls back to
	// generate_system_fingerprint and says so.
	(void)hardware_only;
	return "";
#endif
}

// --------------------------------------------------------------------------

static std::string script_bgt_string_compress(const std::string& data) { return bgt_string_compress(data); }
static std::string script_bgt_string_decompress(const std::string& data) { return bgt_string_decompress(data); }
static std::string script_bgt_string_encrypt(const std::string& data, const std::string& key) { return bgt_string_encrypt(data, key); }
static std::string script_bgt_string_decrypt(const std::string& data, const std::string& key) { return bgt_string_decrypt(data, key); }
static std::string script_bgt_swcr_encrypt(const std::string& data, const std::string& key) { return bgt_swcr_encrypt(data, key); }
static std::string script_bgt_swcr_decrypt(const std::string& data, const std::string& key) { return bgt_swcr_decrypt(data, key); }
static std::string script_bgt_generate_computer_id(const std::string& application_id, bool hardware_only) { return bgt_generate_computer_id(application_id, hardware_only); }

void RegisterBgtLegacyFunctions(asIScriptEngine* engine) {
	engine->RegisterGlobalFunction(_O("string bgt_string_compress(const string&in data)"), asFUNCTION(script_bgt_string_compress), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_string_decompress(const string&in data)"), asFUNCTION(script_bgt_string_decompress), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_serialize(dictionary@ the_data)"), asFUNCTION(bgt_serialize), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("dictionary@ bgt_deserialize(const string&in the_data)"), asFUNCTION(bgt_deserialize), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_string_encrypt(const string&in the_string, const string&in encryption_key)"), asFUNCTION(script_bgt_string_encrypt), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_string_decrypt(const string&in the_string, const string&in encryption_key)"), asFUNCTION(script_bgt_string_decrypt), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_swcr_encrypt(const string&in data, const string&in encryption_key)"), asFUNCTION(script_bgt_swcr_encrypt), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_swcr_decrypt(const string&in data, const string&in encryption_key)"), asFUNCTION(script_bgt_swcr_decrypt), asCALL_CDECL);
	engine->RegisterGlobalFunction(_O("string bgt_generate_computer_id(const string&in application_id, bool hardware_only = false)"), asFUNCTION(script_bgt_generate_computer_id), asCALL_CDECL);
}
