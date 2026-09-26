/* opus.cpp - comprehensive Opus codec plugin for NVGT
 * Provides raw packet encode/decode (opus_encoder, opus_decoder) and
 * OggOpus file I/O (opus_file_reader, opus_file_writer).
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Script API summary:
 *
 *   opus_encoder@ enc(int sample_rate=48000, int channels=1, int application=OPUS_APPLICATION_VOIP)
 *     string  encode(const string &in pcm_int16)
 *     string  encode_float(const string &in pcm_float32)
 *     bool    set_bitrate(int bps)          int  bitrate          (property)
 *     bool    set_complexity(int 0-10)      int  complexity       (property)
 *     bool    set_vbr(bool)                 bool vbr              (property)
 *     bool    set_vbr_constraint(bool)      bool vbr_constraint   (property)
 *     bool    set_dtx(bool)                 bool dtx              (property)
 *     bool    set_inband_fec(bool)          bool inband_fec       (property)
 *     bool    set_expected_loss(int 0-100)  int  expected_loss    (property)
 *     bool    set_signal(int)               int  signal           (property)
 *     bool    set_bandwidth(int)            int  bandwidth        (property)
 *     bool    reset_state()
 *     bool    valid        (property)
 *     string  last_error   (property)
 *     int     sample_rate, channels (const properties)
 *
 *   opus_decoder@ dec(int sample_rate=48000, int channels=1)
 *     string  decode(const string &in packet, int frame_size=960)
 *     string  decode_float(const string &in packet, int frame_size=960)
 *     string  conceal(int frame_size=960)
 *     bool    set_gain(int q8)   int  gain                  (property)
 *     int     last_packet_duration                           (property)
 *     int     bandwidth                                      (property)
 *     bool    reset_state()
 *     bool    valid        (property)
 *     string  last_error   (property)
 *     int     sample_rate, channels (const properties)
 *
 *   Global packet utilities:
 *     int opus_packet_frames(const string &in packet)
 *     int opus_packet_samples(const string &in packet, int sample_rate)
 *     int opus_packet_bandwidth(const string &in packet)
 *     int opus_packet_channels(const string &in packet)
 *
 *   opus_file_reader@ reader(const string &in path)
 *     string  read(int samples_per_channel)
 *     string  read_float(int samples_per_channel)
 *     string  read_all()
 *     string  read_all_float()
 *     bool    seek(int64 sample_offset)
 *     int64   tell()
 *     int64   pcm_total    (property)
 *     int     channels     (property)
 *     int     sample_rate  (property, always 48000)
 *     bool    valid        (property)
 *     string  last_error   (property)
 *
 *   opus_file_writer@ writer(const string &in path, int sample_rate=48000, int channels=1)
 *     bool    write(const string &in pcm_int16)
 *     bool    write_float(const string &in pcm_float32)
 *     bool    close()
 *     bool    add_comment(const string &in tag, const string &in value)
 *     bool    set_bitrate(int bps)
 *     bool    set_complexity(int 0-10)
 *     bool    set_vbr(bool)
 *     bool    set_cvbr(bool)
 *     bool    set_dtx(bool)
 *     bool    set_application(int)
 *     bool    set_signal(int)
 *     bool    valid        (property)
 *     string  last_error   (property)
 *     int     sample_rate, channels (const properties)
 *
 * Note: the engine already registers opus_application_type and opus_signal_type enums
 * (OPUS_APPLICATION_VOIP=2048, OPUS_APPLICATION_AUDIO=2049,
 *  OPUS_APPLICATION_RESTRICTED_LOWDELAY=2051, OPUS_AUTO=-1000,
 *  OPUS_SIGNAL_VOICE=3001, OPUS_SIGNAL_MUSIC=3002).
 * Bandwidth constants from opus.h: OPUS_BANDWIDTH_NARROWBAND=1101,
 *  OPUS_BANDWIDTH_MEDIUMBAND=1102, OPUS_BANDWIDTH_WIDEBAND=1103,
 *  OPUS_BANDWIDTH_SUPERWIDEBAND=1104, OPUS_BANDWIDTH_FULLBAND=1105,
 *  OPUS_AUTO=-1000 (no bandwidth limit).
 */

#include "../../src/nvgt_plugin.h"
#include <opus/opus.h>
#include <opus/opusfile.h>
#include <opus/opusenc.h>
#include <atomic>
#include <string>
#include <vector>

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string opusfile_strerror(int err) {
	switch (err) {
		case 0:              return "success";
		case OP_FALSE:       return "request was not fulfilled";
		case OP_HOLE:        return "hole detected in data (recoverable)";
		case OP_EREAD:       return "read error";
		case OP_EFAULT:      return "internal fault";
		case OP_EIMPL:       return "not implemented";
		case OP_EINVAL:      return "invalid argument";
		case OP_ENOTFORMAT:  return "not an Ogg Opus stream";
		case OP_EBADHEADER:  return "bad Opus header";
		case OP_EVERSION:    return "unsupported bitstream version";
		case OP_ENOTAUDIO:   return "stream contains no audio data";
		case OP_EBADPACKET:  return "bad packet";
		case OP_EBADLINK:    return "bad link in chained stream";
		case OP_ENOSEEK:     return "stream is not seekable";
		case OP_EBADTIMESTAMP: return "bad timestamp";
		default:             return "unknown opusfile error: " + std::to_string(err);
	}
}

// ── opus_encoder ──────────────────────────────────────────────────────────────

class nvgt_opus_encoder {
	OpusEncoder* enc;
	std::atomic<int> ref_count;
	int last_err;
public:
	const int sample_rate;
	const int channels;

	nvgt_opus_encoder(int sr, int ch, int app)
		: enc(nullptr), ref_count(1), last_err(OPUS_OK), sample_rate(sr), channels(ch) {
		enc = opus_encoder_create(sr, ch, app, &last_err);
		if (last_err != OPUS_OK) enc = nullptr;
	}
	~nvgt_opus_encoder() { if (enc) opus_encoder_destroy(enc); }

	void add_ref() { ++ref_count; }
	void release() { if (--ref_count <= 0) delete this; }
	bool is_valid() const { return enc != nullptr; }
	std::string get_last_error() const { return opus_strerror(last_err); }

	std::string encode(const std::string& pcm_int16) {
		if (!enc || pcm_int16.empty()) return "";
		int frame_size = (int)(pcm_int16.size() / sizeof(opus_int16)) / channels;
		std::string out(4000, '\0');
		opus_int32 bytes = opus_encode(enc,
			reinterpret_cast<const opus_int16*>(pcm_int16.data()), frame_size,
			reinterpret_cast<unsigned char*>(&out[0]), (opus_int32)out.size());
		if (bytes <= 0) { last_err = (int)bytes; return ""; }
		out.resize(bytes);
		return out;
	}

	std::string encode_float(const std::string& pcm_f32) {
		if (!enc || pcm_f32.empty()) return "";
		int frame_size = (int)(pcm_f32.size() / sizeof(float)) / channels;
		std::string out(4000, '\0');
		opus_int32 bytes = opus_encode_float(enc,
			reinterpret_cast<const float*>(pcm_f32.data()), frame_size,
			reinterpret_cast<unsigned char*>(&out[0]), (opus_int32)out.size());
		if (bytes <= 0) { last_err = (int)bytes; return ""; }
		out.resize(bytes);
		return out;
	}

	bool set_bitrate(int bps) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_BITRATE(bps));
		return last_err == OPUS_OK;
	}
	int get_bitrate() const {
		if (!enc) return -1;
		opus_int32 v = -1; opus_encoder_ctl(enc, OPUS_GET_BITRATE(&v)); return (int)v;
	}

	bool set_complexity(int c) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c));
		return last_err == OPUS_OK;
	}
	int get_complexity() const {
		if (!enc) return -1;
		opus_int32 v = -1; opus_encoder_ctl(enc, OPUS_GET_COMPLEXITY(&v)); return (int)v;
	}

	bool set_vbr(bool on) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_VBR(on ? 1 : 0));
		return last_err == OPUS_OK;
	}
	bool get_vbr() const {
		if (!enc) return false;
		opus_int32 v = 0; opus_encoder_ctl(enc, OPUS_GET_VBR(&v)); return v != 0;
	}

	bool set_vbr_constraint(bool on) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(on ? 1 : 0));
		return last_err == OPUS_OK;
	}
	bool get_vbr_constraint() const {
		if (!enc) return false;
		opus_int32 v = 0; opus_encoder_ctl(enc, OPUS_GET_VBR_CONSTRAINT(&v)); return v != 0;
	}

	bool set_dtx(bool on) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_DTX(on ? 1 : 0));
		return last_err == OPUS_OK;
	}
	bool get_dtx() const {
		if (!enc) return false;
		opus_int32 v = 0; opus_encoder_ctl(enc, OPUS_GET_DTX(&v)); return v != 0;
	}

	bool set_inband_fec(bool on) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(on ? 1 : 0));
		return last_err == OPUS_OK;
	}
	bool get_inband_fec() const {
		if (!enc) return false;
		opus_int32 v = 0; opus_encoder_ctl(enc, OPUS_GET_INBAND_FEC(&v)); return v != 0;
	}

	bool set_expected_loss(int pct) {
		if (!enc || pct < 0 || pct > 100) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(pct));
		return last_err == OPUS_OK;
	}
	int get_expected_loss() const {
		if (!enc) return -1;
		opus_int32 v = 0; opus_encoder_ctl(enc, OPUS_GET_PACKET_LOSS_PERC(&v)); return (int)v;
	}

	bool set_signal(int sig) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_SIGNAL(sig));
		return last_err == OPUS_OK;
	}
	int get_signal() const {
		if (!enc) return OPUS_AUTO;
		opus_int32 v = OPUS_AUTO; opus_encoder_ctl(enc, OPUS_GET_SIGNAL(&v)); return (int)v;
	}

	bool set_bandwidth(int bw) {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(bw));
		return last_err == OPUS_OK;
	}
	int get_bandwidth() const {
		if (!enc) return -1;
		opus_int32 v = -1; opus_encoder_ctl(enc, OPUS_GET_BANDWIDTH(&v)); return (int)v;
	}

	bool reset_state() {
		if (!enc) return false;
		last_err = opus_encoder_ctl(enc, OPUS_RESET_STATE);
		return last_err == OPUS_OK;
	}
};

// ── opus_decoder ──────────────────────────────────────────────────────────────

class nvgt_opus_decoder {
	OpusDecoder* dec;
	std::atomic<int> ref_count;
	int last_err;
public:
	const int sample_rate;
	const int channels;

	nvgt_opus_decoder(int sr, int ch)
		: dec(nullptr), ref_count(1), last_err(OPUS_OK), sample_rate(sr), channels(ch) {
		dec = opus_decoder_create(sr, ch, &last_err);
		if (last_err != OPUS_OK) dec = nullptr;
	}
	~nvgt_opus_decoder() { if (dec) opus_decoder_destroy(dec); }

	void add_ref() { ++ref_count; }
	void release() { if (--ref_count <= 0) delete this; }
	bool is_valid() const { return dec != nullptr; }
	std::string get_last_error() const { return opus_strerror(last_err); }

	std::string decode(const std::string& packet, int frame_size = 960) {
		if (!dec) return "";
		std::string out(frame_size * channels * (int)sizeof(opus_int16), '\0');
		int samples = opus_decode(dec,
			reinterpret_cast<const unsigned char*>(packet.data()), (opus_int32)packet.size(),
			reinterpret_cast<opus_int16*>(&out[0]), frame_size, 0);
		if (samples <= 0) { last_err = samples; return ""; }
		out.resize(samples * channels * sizeof(opus_int16));
		return out;
	}

	std::string decode_float(const std::string& packet, int frame_size = 960) {
		if (!dec) return "";
		std::string out(frame_size * channels * (int)sizeof(float), '\0');
		int samples = opus_decode_float(dec,
			reinterpret_cast<const unsigned char*>(packet.data()), (opus_int32)packet.size(),
			reinterpret_cast<float*>(&out[0]), frame_size, 0);
		if (samples <= 0) { last_err = samples; return ""; }
		out.resize(samples * channels * sizeof(float));
		return out;
	}

	std::string conceal(int frame_size = 960) {
		if (!dec) return "";
		std::string out(frame_size * channels * (int)sizeof(float), '\0');
		int samples = opus_decode_float(dec, nullptr, 0,
			reinterpret_cast<float*>(&out[0]), frame_size, 0);
		if (samples <= 0) { last_err = samples; return ""; }
		out.resize(samples * channels * sizeof(float));
		return out;
	}

	bool set_gain(int gain_q8) {
		if (!dec) return false;
		last_err = opus_decoder_ctl(dec, OPUS_SET_GAIN(gain_q8));
		return last_err == OPUS_OK;
	}
	int get_gain() const {
		if (!dec) return 0;
		opus_int32 v = 0; opus_decoder_ctl(dec, OPUS_GET_GAIN(&v)); return (int)v;
	}

	int get_last_packet_duration() const {
		if (!dec) return 0;
		opus_int32 v = 0; opus_decoder_ctl(dec, OPUS_GET_LAST_PACKET_DURATION(&v)); return (int)v;
	}

	int get_bandwidth() const {
		if (!dec) return -1;
		opus_int32 v = -1; opus_decoder_ctl(dec, OPUS_GET_BANDWIDTH(&v)); return (int)v;
	}

	bool reset_state() {
		if (!dec) return false;
		last_err = opus_decoder_ctl(dec, OPUS_RESET_STATE);
		return last_err == OPUS_OK;
	}
};

// ── packet utilities ──────────────────────────────────────────────────────────

static int packet_frames(const std::string& pkt) {
	if (pkt.empty()) return OPUS_INVALID_PACKET;
	return opus_packet_get_nb_frames(
		reinterpret_cast<const unsigned char*>(pkt.data()), (opus_int32)pkt.size());
}
static int packet_samples(const std::string& pkt, int sample_rate) {
	if (pkt.empty()) return OPUS_INVALID_PACKET;
	return opus_packet_get_nb_samples(
		reinterpret_cast<const unsigned char*>(pkt.data()), (opus_int32)pkt.size(), sample_rate);
}
static int packet_bandwidth(const std::string& pkt) {
	if (pkt.empty()) return OPUS_INVALID_PACKET;
	return opus_packet_get_bandwidth(
		reinterpret_cast<const unsigned char*>(pkt.data()));
}
static int packet_channels(const std::string& pkt) {
	if (pkt.empty()) return OPUS_INVALID_PACKET;
	return opus_packet_get_nb_channels(
		reinterpret_cast<const unsigned char*>(pkt.data()));
}

// ── opus_file_reader ──────────────────────────────────────────────────────────

class nvgt_opus_file_reader {
	OggOpusFile* of;
	std::atomic<int> ref_count;
	int last_err;
public:
	nvgt_opus_file_reader(const std::string& path)
		: of(nullptr), ref_count(1), last_err(0) {
		of = op_open_file(path.c_str(), &last_err);
	}
	~nvgt_opus_file_reader() { if (of) op_free(of); }

	void add_ref() { ++ref_count; }
	void release() { if (--ref_count <= 0) delete this; }
	bool is_valid() const { return of != nullptr; }
	std::string get_last_error() const { return opusfile_strerror(last_err); }

	int get_channels() const { return of ? op_channel_count(of, -1) : 0; }
	int get_sample_rate() const { return 48000; }

	int64_t get_pcm_total() const {
		return of ? (int64_t)op_pcm_total(of, -1) : -1;
	}

	int64_t tell_pos() const {
		return of ? (int64_t)op_pcm_tell(of) : -1;
	}

	bool seek(int64_t sample_offset) {
		if (!of) return false;
		last_err = op_pcm_seek(of, (ogg_int64_t)sample_offset);
		return last_err == 0;
	}

	// Returns int16 LE interleaved PCM for up to samples_per_channel samples.
	std::string read(int samples_per_channel) {
		if (!of || samples_per_channel <= 0) return "";
		int ch = op_channel_count(of, -1);
		if (ch <= 0) return "";
		std::vector<opus_int16> buf((size_t)samples_per_channel * ch);
		int total = 0;
		while (total < samples_per_channel) {
			int n = op_read(of, buf.data() + (size_t)total * ch,
				(samples_per_channel - total) * ch, nullptr);
			if (n < 0) { last_err = n; break; }
			if (n == 0) break;
			total += n;
		}
		return std::string(reinterpret_cast<const char*>(buf.data()),
			(size_t)total * ch * sizeof(opus_int16));
	}

	// Returns float32 LE interleaved PCM for up to samples_per_channel samples.
	std::string read_float(int samples_per_channel) {
		if (!of || samples_per_channel <= 0) return "";
		int ch = op_channel_count(of, -1);
		if (ch <= 0) return "";
		std::vector<float> buf((size_t)samples_per_channel * ch);
		int total = 0;
		while (total < samples_per_channel) {
			int n = op_read_float(of, buf.data() + (size_t)total * ch,
				(samples_per_channel - total) * ch, nullptr);
			if (n < 0) { last_err = n; break; }
			if (n == 0) break;
			total += n;
		}
		return std::string(reinterpret_cast<const char*>(buf.data()),
			(size_t)total * ch * sizeof(float));
	}

	std::string read_all() {
		if (!of) return "";
		ogg_int64_t total = op_pcm_total(of, -1);
		return read(total >= 0 ? (int)total : 48000 * 3600);
	}

	std::string read_all_float() {
		if (!of) return "";
		ogg_int64_t total = op_pcm_total(of, -1);
		return read_float(total >= 0 ? (int)total : 48000 * 3600);
	}
};

// ── opus_file_writer ──────────────────────────────────────────────────────────

class nvgt_opus_file_writer {
	OggOpusEnc* enc;
	OggOpusComments* comments;
	std::atomic<int> ref_count;
	int last_err;
	bool closed;
public:
	const int sample_rate;
	const int channels;

	nvgt_opus_file_writer(const std::string& path, int sr, int ch)
		: enc(nullptr), comments(nullptr), ref_count(1),
		  last_err(OPE_OK), closed(false), sample_rate(sr), channels(ch) {
		comments = ope_comments_create();
		enc = ope_encoder_create_file(path.c_str(), comments, sr, ch, 0, &last_err);
		if (last_err != OPE_OK) enc = nullptr;
	}
	~nvgt_opus_file_writer() {
		if (enc) { if (!closed) ope_encoder_drain(enc); ope_encoder_destroy(enc); }
		if (comments) ope_comments_destroy(comments);
	}

	void add_ref() { ++ref_count; }
	void release() { if (--ref_count <= 0) delete this; }
	bool is_valid() const { return enc != nullptr && !closed; }
	std::string get_last_error() const { return ope_strerror(last_err); }

	// Must be called before the first write for tags to appear in the file.
	bool add_comment(const std::string& tag, const std::string& value) {
		if (!comments) return false;
		last_err = ope_comments_add(comments, tag.c_str(), value.c_str());
		return last_err == OPE_OK;
	}

	bool write(const std::string& pcm_int16) {
		if (!enc || closed || pcm_int16.empty()) return false;
		int spc = (int)(pcm_int16.size() / sizeof(opus_int16)) / channels;
		last_err = ope_encoder_write(enc,
			reinterpret_cast<const opus_int16*>(pcm_int16.data()), spc);
		return last_err == OPE_OK;
	}

	bool write_float(const std::string& pcm_f32) {
		if (!enc || closed || pcm_f32.empty()) return false;
		int spc = (int)(pcm_f32.size() / sizeof(float)) / channels;
		last_err = ope_encoder_write_float(enc,
			reinterpret_cast<const float*>(pcm_f32.data()), spc);
		return last_err == OPE_OK;
	}

	// Flush and finalize the file. The object becomes invalid after this.
	bool close() {
		if (!enc || closed) return false;
		last_err = ope_encoder_drain(enc);
		if (last_err == OPE_OK) closed = true;
		return last_err == OPE_OK;
	}

	bool set_bitrate(int bps) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_BITRATE(bps));
		return last_err == OPE_OK;
	}
	bool set_complexity(int c) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c));
		return last_err == OPE_OK;
	}
	bool set_vbr(bool on) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_VBR(on ? 1 : 0));
		return last_err == OPE_OK;
	}
	bool set_cvbr(bool on) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(on ? 1 : 0));
		return last_err == OPE_OK;
	}
	bool set_dtx(bool on) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_DTX(on ? 1 : 0));
		return last_err == OPE_OK;
	}
	bool set_application(int app) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_APPLICATION(app));
		return last_err == OPE_OK;
	}
	bool set_signal(int sig) {
		if (!enc || closed) return false;
		last_err = ope_encoder_ctl(enc, OPUS_SET_SIGNAL(sig));
		return last_err == OPE_OK;
	}
};

// ── factories ─────────────────────────────────────────────────────────────────

static nvgt_opus_encoder* encoder_factory(int sr, int ch, int app) {
	return new nvgt_opus_encoder(sr, ch, app);
}
static nvgt_opus_decoder* decoder_factory(int sr, int ch) {
	return new nvgt_opus_decoder(sr, ch);
}
static nvgt_opus_file_reader* file_reader_factory(const std::string& path) {
	return new nvgt_opus_file_reader(path);
}
static nvgt_opus_file_writer* file_writer_factory(const std::string& path, int sr, int ch) {
	return new nvgt_opus_file_writer(path, sr, ch);
}

// ── plugin entry point ────────────────────────────────────────────────────────

plugin_main(nvgt_plugin_shared* shared) {
	prepare_plugin(shared);
	asIScriptEngine* e = shared->script_engine;

	// ── opus_encoder ──────────────────────────────────────────────────────────
	e->RegisterObjectType("opus_encoder", 0, asOBJ_REF);
	e->RegisterObjectBehaviour("opus_encoder", asBEHAVE_FACTORY,
		"opus_encoder@ f(int sample_rate = 48000, int channels = 1, int application = 2048)",
		asFUNCTION(encoder_factory), asCALL_CDECL);
	e->RegisterObjectBehaviour("opus_encoder", asBEHAVE_ADDREF,
		"void f()", asMETHOD(nvgt_opus_encoder, add_ref), asCALL_THISCALL);
	e->RegisterObjectBehaviour("opus_encoder", asBEHAVE_RELEASE,
		"void f()", asMETHOD(nvgt_opus_encoder, release), asCALL_THISCALL);

	e->RegisterObjectMethod("opus_encoder", "string encode(const string &in pcm_int16)",
		asMETHOD(nvgt_opus_encoder, encode), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "string encode_float(const string &in pcm_float32)",
		asMETHOD(nvgt_opus_encoder, encode_float), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_bitrate(int bps)",
		asMETHOD(nvgt_opus_encoder, set_bitrate), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "int get_bitrate() const property",
		asMETHOD(nvgt_opus_encoder, get_bitrate), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_complexity(int c)",
		asMETHOD(nvgt_opus_encoder, set_complexity), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "int get_complexity() const property",
		asMETHOD(nvgt_opus_encoder, get_complexity), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_vbr(bool on)",
		asMETHOD(nvgt_opus_encoder, set_vbr), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool get_vbr() const property",
		asMETHOD(nvgt_opus_encoder, get_vbr), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_vbr_constraint(bool on)",
		asMETHOD(nvgt_opus_encoder, set_vbr_constraint), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool get_vbr_constraint() const property",
		asMETHOD(nvgt_opus_encoder, get_vbr_constraint), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_dtx(bool on)",
		asMETHOD(nvgt_opus_encoder, set_dtx), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool get_dtx() const property",
		asMETHOD(nvgt_opus_encoder, get_dtx), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_inband_fec(bool on)",
		asMETHOD(nvgt_opus_encoder, set_inband_fec), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool get_inband_fec() const property",
		asMETHOD(nvgt_opus_encoder, get_inband_fec), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_expected_loss(int pct)",
		asMETHOD(nvgt_opus_encoder, set_expected_loss), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "int get_expected_loss() const property",
		asMETHOD(nvgt_opus_encoder, get_expected_loss), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_signal(int sig)",
		asMETHOD(nvgt_opus_encoder, set_signal), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "int get_signal() const property",
		asMETHOD(nvgt_opus_encoder, get_signal), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool set_bandwidth(int bw)",
		asMETHOD(nvgt_opus_encoder, set_bandwidth), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "int get_bandwidth() const property",
		asMETHOD(nvgt_opus_encoder, get_bandwidth), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool reset_state()",
		asMETHOD(nvgt_opus_encoder, reset_state), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "bool get_valid() const property",
		asMETHOD(nvgt_opus_encoder, is_valid), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_encoder", "string get_last_error() const property",
		asMETHOD(nvgt_opus_encoder, get_last_error), asCALL_THISCALL);
	e->RegisterObjectProperty("opus_encoder", "const int sample_rate",
		asOFFSET(nvgt_opus_encoder, sample_rate));
	e->RegisterObjectProperty("opus_encoder", "const int channels",
		asOFFSET(nvgt_opus_encoder, channels));

	// ── opus_decoder ──────────────────────────────────────────────────────────
	e->RegisterObjectType("opus_decoder", 0, asOBJ_REF);
	e->RegisterObjectBehaviour("opus_decoder", asBEHAVE_FACTORY,
		"opus_decoder@ f(int sample_rate = 48000, int channels = 1)",
		asFUNCTION(decoder_factory), asCALL_CDECL);
	e->RegisterObjectBehaviour("opus_decoder", asBEHAVE_ADDREF,
		"void f()", asMETHOD(nvgt_opus_decoder, add_ref), asCALL_THISCALL);
	e->RegisterObjectBehaviour("opus_decoder", asBEHAVE_RELEASE,
		"void f()", asMETHOD(nvgt_opus_decoder, release), asCALL_THISCALL);

	e->RegisterObjectMethod("opus_decoder", "string decode(const string &in packet, int frame_size = 960)",
		asMETHOD(nvgt_opus_decoder, decode), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "string decode_float(const string &in packet, int frame_size = 960)",
		asMETHOD(nvgt_opus_decoder, decode_float), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "string conceal(int frame_size = 960)",
		asMETHOD(nvgt_opus_decoder, conceal), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "bool set_gain(int q8)",
		asMETHOD(nvgt_opus_decoder, set_gain), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "int get_gain() const property",
		asMETHOD(nvgt_opus_decoder, get_gain), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "int get_last_packet_duration() const property",
		asMETHOD(nvgt_opus_decoder, get_last_packet_duration), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "int get_bandwidth() const property",
		asMETHOD(nvgt_opus_decoder, get_bandwidth), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "bool reset_state()",
		asMETHOD(nvgt_opus_decoder, reset_state), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "bool get_valid() const property",
		asMETHOD(nvgt_opus_decoder, is_valid), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_decoder", "string get_last_error() const property",
		asMETHOD(nvgt_opus_decoder, get_last_error), asCALL_THISCALL);
	e->RegisterObjectProperty("opus_decoder", "const int sample_rate",
		asOFFSET(nvgt_opus_decoder, sample_rate));
	e->RegisterObjectProperty("opus_decoder", "const int channels",
		asOFFSET(nvgt_opus_decoder, channels));

	// ── packet utilities ──────────────────────────────────────────────────────
	e->RegisterGlobalFunction("int opus_packet_frames(const string &in packet)",
		asFUNCTION(packet_frames), asCALL_CDECL);
	e->RegisterGlobalFunction("int opus_packet_samples(const string &in packet, int sample_rate)",
		asFUNCTION(packet_samples), asCALL_CDECL);
	e->RegisterGlobalFunction("int opus_packet_bandwidth(const string &in packet)",
		asFUNCTION(packet_bandwidth), asCALL_CDECL);
	e->RegisterGlobalFunction("int opus_packet_channels(const string &in packet)",
		asFUNCTION(packet_channels), asCALL_CDECL);

	// ── opus_file_reader ──────────────────────────────────────────────────────
	e->RegisterObjectType("opus_file_reader", 0, asOBJ_REF);
	e->RegisterObjectBehaviour("opus_file_reader", asBEHAVE_FACTORY,
		"opus_file_reader@ f(const string &in path)",
		asFUNCTION(file_reader_factory), asCALL_CDECL);
	e->RegisterObjectBehaviour("opus_file_reader", asBEHAVE_ADDREF,
		"void f()", asMETHOD(nvgt_opus_file_reader, add_ref), asCALL_THISCALL);
	e->RegisterObjectBehaviour("opus_file_reader", asBEHAVE_RELEASE,
		"void f()", asMETHOD(nvgt_opus_file_reader, release), asCALL_THISCALL);

	e->RegisterObjectMethod("opus_file_reader", "string read(int samples_per_channel)",
		asMETHOD(nvgt_opus_file_reader, read), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "string read_float(int samples_per_channel)",
		asMETHOD(nvgt_opus_file_reader, read_float), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "string read_all()",
		asMETHOD(nvgt_opus_file_reader, read_all), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "string read_all_float()",
		asMETHOD(nvgt_opus_file_reader, read_all_float), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "bool seek(int64 sample_offset)",
		asMETHOD(nvgt_opus_file_reader, seek), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "int64 tell()",
		asMETHOD(nvgt_opus_file_reader, tell_pos), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "int64 get_pcm_total() const property",
		asMETHOD(nvgt_opus_file_reader, get_pcm_total), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "int get_channels() const property",
		asMETHOD(nvgt_opus_file_reader, get_channels), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "int get_sample_rate() const property",
		asMETHOD(nvgt_opus_file_reader, get_sample_rate), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "bool get_valid() const property",
		asMETHOD(nvgt_opus_file_reader, is_valid), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_reader", "string get_last_error() const property",
		asMETHOD(nvgt_opus_file_reader, get_last_error), asCALL_THISCALL);

	// ── opus_file_writer ──────────────────────────────────────────────────────
	e->RegisterObjectType("opus_file_writer", 0, asOBJ_REF);
	e->RegisterObjectBehaviour("opus_file_writer", asBEHAVE_FACTORY,
		"opus_file_writer@ f(const string &in path, int sample_rate = 48000, int channels = 1)",
		asFUNCTION(file_writer_factory), asCALL_CDECL);
	e->RegisterObjectBehaviour("opus_file_writer", asBEHAVE_ADDREF,
		"void f()", asMETHOD(nvgt_opus_file_writer, add_ref), asCALL_THISCALL);
	e->RegisterObjectBehaviour("opus_file_writer", asBEHAVE_RELEASE,
		"void f()", asMETHOD(nvgt_opus_file_writer, release), asCALL_THISCALL);

	e->RegisterObjectMethod("opus_file_writer", "bool write(const string &in pcm_int16)",
		asMETHOD(nvgt_opus_file_writer, write), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool write_float(const string &in pcm_float32)",
		asMETHOD(nvgt_opus_file_writer, write_float), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool close()",
		asMETHOD(nvgt_opus_file_writer, close), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool add_comment(const string &in tag, const string &in value)",
		asMETHOD(nvgt_opus_file_writer, add_comment), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_bitrate(int bps)",
		asMETHOD(nvgt_opus_file_writer, set_bitrate), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_complexity(int c)",
		asMETHOD(nvgt_opus_file_writer, set_complexity), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_vbr(bool on)",
		asMETHOD(nvgt_opus_file_writer, set_vbr), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_cvbr(bool on)",
		asMETHOD(nvgt_opus_file_writer, set_cvbr), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_dtx(bool on)",
		asMETHOD(nvgt_opus_file_writer, set_dtx), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_application(int app)",
		asMETHOD(nvgt_opus_file_writer, set_application), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool set_signal(int sig)",
		asMETHOD(nvgt_opus_file_writer, set_signal), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "bool get_valid() const property",
		asMETHOD(nvgt_opus_file_writer, is_valid), asCALL_THISCALL);
	e->RegisterObjectMethod("opus_file_writer", "string get_last_error() const property",
		asMETHOD(nvgt_opus_file_writer, get_last_error), asCALL_THISCALL);
	e->RegisterObjectProperty("opus_file_writer", "const int sample_rate",
		asOFFSET(nvgt_opus_file_writer, sample_rate));
	e->RegisterObjectProperty("opus_file_writer", "const int channels",
		asOFFSET(nvgt_opus_file_writer, channels));

	return true;
}
