/* sound_nodes.cpp - audio nodes implementation
 * This contains code for hooking all effects and other nodes up to miniaudio, from hrtf to reverb to filters to tone synthesis and more.
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <Poco/NotificationQueue.h>
#include <Poco/Thread.h>
#include <ma_reverb_node.h>
#include "misc_functions.h" // range_convert
#include "sound_nodes.h"

using namespace std;

// This node allows easy creation of miniaudio nodes in C++.
static void ma_effect_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut) {
	ma_effect_node* node = (ma_effect_node*)pNode;
	if (!node->node) return;
	node->node->process(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
}
static ma_result ma_effect_node_get_required_input_frame_count(ma_node* pNode, ma_uint32 outputFrameCount, ma_uint32* input_frame_count) {
	ma_effect_node* node = (ma_effect_node*)pNode;
	if (!node->node) return MA_ERROR;
	*input_frame_count = node->node->required_input_frame_count(outputFrameCount);
	return MA_SUCCESS;
}
effect_node_impl::effect_node_impl(audio_engine* e, ma_uint8 input_channel_count, ma_uint8 output_channel_count, ma_uint8 input_bus_count, ma_uint8 output_bus_count, unsigned int flags) : n(make_unique<ma_effect_node>()), audio_node_impl(nullptr, e), vtable({&ma_effect_node_process_pcm_frames, &ma_effect_node_get_required_input_frame_count, input_bus_count, output_bus_count, flags}) {
	if (!input_channel_count) input_channel_count = e->get_channels();
	if (!output_channel_count) output_channel_count = e->get_channels();
	ma_node_config cfg = ma_node_config_init();
	vector<ma_uint32> channels_in(input_bus_count, input_channel_count), channels_out(output_bus_count, output_channel_count);
	cfg.vtable          = &vtable;
	if (input_bus_count > 0) cfg.pInputChannels  = &channels_in[0];
	if (output_bus_count > 0) cfg.pOutputChannels = &channels_out[0];
	if ((g_soundsystem_last_error = ma_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, (ma_node_base*)&*n)) != MA_SUCCESS) throw std::runtime_error("failed to create effect node");
	n->node = this;
	node = (ma_node_base*)&*n;
}
effect_node_impl::~effect_node_impl() { destroy_node(); }
void effect_node_impl::destroy_node() {
	if (n) ma_node_uninit((ma_node_base*)&*n, nullptr);
	n.reset();
}
void effect_node_impl::process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) {} // override in subclasses.
unsigned int effect_node_impl::required_input_frame_count(unsigned int output_frame_count) const { return output_frame_count; }

// The following node acts as a simple passthrough, with a callback that does nothing. The purpose is for any object that exists between or in any way handles nodes to be able to exist in the node graph.
// For example a reverb3d node acts as a high level API to applying reverb to 3d sounds. We want the user to be able to swap underlying reverb effect nodes that all sounds attached to the reverb3d objects are using, but prefferably without keeping track of sounds to reattach. Therefor, reverb3d acts as a passthrough node which all connected sounds are attached to, allowing us to swap the underlying reverb effect in one place rather than for all connected sounds.
class passthrough_node_impl : public effect_node_impl, public virtual passthrough_node {
	public:
	passthrough_node_impl(audio_engine* e) : effect_node_impl(e, 0, 0, 1, 1, MA_NODE_FLAG_PASSTHROUGH | MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT) {}
};
passthrough_node* passthrough_node::create(audio_engine* engine) { return new passthrough_node_impl(engine); }

class audio_node_chain_impl : public passthrough_node_impl, public virtual audio_node_chain {
	audio_node* source;
	std::vector<audio_node*> nodes;
	audio_node* endpoint;
	unsigned int endpoint_input_bus_index;
public:
	audio_node_chain_impl(audio_node* source, audio_node* endpoint, audio_engine* e) : passthrough_node_impl(e), endpoint(endpoint) {
		if (source) source->attach_output_bus(0, this, 0);
		if (endpoint) attach_output_bus(0, endpoint, 0);
	}
	~audio_node_chain_impl() {
		// We only release references, all attachments are kept in tact. Call clear(true) to detach all known nodes instead.
		// endpoint is not released here: whichever node is physically wired to it (this, if the chain was empty, or the last node otherwise) owns that reference via audio_node_impl's own attach/detach bookkeeping.
		for (audio_node* node: nodes) node->release();
	}
	bool attach_output_bus(unsigned int bus_index, audio_node* node, unsigned int input_bus_index) override {
		set_endpoint(node, input_bus_index);
		return endpoint == node;
	}
	bool detach_output_bus(unsigned int bus_index) override { set_endpoint(nullptr, 0); return endpoint == nullptr; }
	bool detach_all_output_buses() override { return detach_output_bus(0); }
	bool add_node(audio_node* node, audio_node* after, unsigned int input_bus_index) override {
		if (!node) return false;
		unsigned int new_idx = 0;
		if (after) {
			new_idx = index_of(after);
			if (new_idx == -1) return false;
			else new_idx += 1; // Be sure to insert after this position rather than before.
		}
		audio_node* prev = new_idx? nodes[new_idx -1] : nullptr;
		audio_node* next = new_idx? (new_idx < nodes.size()? nodes[new_idx] : endpoint) : (!nodes.empty()? first() : endpoint);
		if (prev && !prev->attach_output_bus(0, node, 0)) return false;
		else if (!prev && !audio_node_impl::attach_output_bus(0, node, 0)) return false;
		if (next && !node->attach_output_bus(0, next, input_bus_index)) return false;
		nodes.insert(nodes.begin() + new_idx, node);
		node->duplicate();
		return true;
	}
	bool add_node_at(audio_node* node, int after, unsigned int input_bus_index) override {
		if (after < -1 || after >= nodes.size()) return false;
		audio_node* insert_after = after > -1? nodes[after] : nullptr;
		return add_node(node, insert_after, input_bus_index);
	}
	bool remove_node(audio_node* node) override {
		if (!node) return false;
		auto it = find(nodes.begin(), nodes.end(), node);
		if (it == nodes.end()) return false;
		audio_node* prev = (*it) != nodes.front()? *(it -1) : nullptr;
		audio_node* next = (*it) != nodes.back()? *(it + 1) : endpoint;
		if (prev && next && !prev->attach_output_bus(0, next, 0)) return false;
		else if (!prev && next && !audio_node_impl::attach_output_bus(0, next, 0)) return false;
		nodes.erase(it);
		bool success = node->detach_output_bus(0);
		node->release();
		return success;
	}
	bool remove_node_at(unsigned int index) override {
		if (index >= nodes.size()) return false;
		return remove_node(nodes[index]);
	}
	bool clear(bool detach_nodes) override {
		bool success = audio_node_impl::detach_output_bus(0);
		for (audio_node* node : nodes) {
			if (success && detach_nodes) success = node->detach_output_bus(0);
			node->release();
		}
		if (success && endpoint) success = audio_node_impl::attach_output_bus(0, endpoint, 0);
		nodes.clear();
		return success;
	}
	void set_endpoint(audio_node* node, unsigned int input_bus_index) override {
		// Ownership of endpoint is not tracked here; it is taken/released automatically by whichever node's attach_output_bus/detach_output_bus is actually called below (audio_node_impl's own bookkeeping).
		if (endpoint) {
			if (!nodes.empty()) last()->detach_output_bus(0);
			else audio_node_impl::detach_output_bus(0);
		}
		endpoint = node;
		if (endpoint) {
			if (!nodes.empty()) last()->attach_output_bus(0, endpoint, input_bus_index);
			else audio_node_impl::attach_output_bus(0, endpoint, input_bus_index);
		}
	}
	audio_node* get_endpoint() const override { return endpoint; }
	audio_node* first() const override {
		if (nodes.empty()) return nullptr;
		return nodes[0];
	}
	audio_node* last() const override {
		if (nodes.empty()) return nullptr;
		return nodes[nodes.size() -1];
	}
	audio_node* operator[](unsigned int index) const override {
		if (nodes.size() <= index) return nullptr;
		return nodes[index];
	}
	int index_of(audio_node* node) const override {
		auto it = find(nodes.begin(), nodes.end(), node);
		if (it == nodes.end()) return -1;
		return distance(nodes.begin(), it);
	}
	unsigned int get_node_count() const override { return nodes.size(); }
};
audio_node_chain* audio_node_chain::create(audio_node* source, audio_node* endpoint, audio_engine* engine) { return new audio_node_chain_impl(source, endpoint, engine); }

// sound_environment helpers used by the phonon attenuator; defined at the end of this file.
bool sound_environment_source_ready(sound_environment_source* s);
bool sound_environment_try_prefill(sound_environment* env, sound_environment_source* s);
void sound_environment_source_get_route_bands(sound_environment_source* s, float bands[3]);

static IPLAudioSettings g_phonon_audio_settings {44100, SOUNDSYSTEM_FRAMESIZE}; // We will update samplerate later in phonon_init.
static IPLContext g_phonon_context = nullptr;
static IPLHRTF g_phonon_hrtf = nullptr;

bool phonon_init() {
	if (g_phonon_context) return true;
	if (!init_sound()) return false;
	g_phonon_audio_settings = {g_audio_engine->get_sample_rate(), SOUNDSYSTEM_FRAMESIZE};
	IPLContextSettings phonon_context_settings{};
	phonon_context_settings.version = STEAMAUDIO_VERSION;
	if (iplContextCreate(&phonon_context_settings, &g_phonon_context) != IPL_STATUS_SUCCESS) return false;
	IPLHRTFSettings phonon_hrtf_settings{};
	phonon_hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
	phonon_hrtf_settings.volume = 1.0;
	if (iplHRTFCreate(g_phonon_context, &g_phonon_audio_settings, &phonon_hrtf_settings, &g_phonon_hrtf) != IPL_STATUS_SUCCESS) {
		iplContextRelease(&g_phonon_context);
		g_phonon_context = nullptr;
		return false;
	}
	return true;
}

bool set_global_hrtf(bool enabled) {
	if (enabled == get_global_hrtf()) return true;
	if (enabled) {
		if (!phonon_init()) return false;
		if (!sound_set_spatialization(g_audio_phonon_hrtf_panner, g_audio_phonon_attenuator)) return false;
	} else return sound_set_spatialization(g_audio_basic_panner, g_audio_basic_attenuator, true, false);
	return true;
}
bool get_global_hrtf() { return get_audio_panner_enabled(g_audio_phonon_hrtf_panner) && get_audio_attenuator_enabled(g_audio_phonon_attenuator); }

class phonon_binaural_node_impl : public audio_node_impl, public virtual phonon_binaural_node {
	unique_ptr<ma_phonon_binaural_node> bn;
	public:
		phonon_binaural_node_impl(audio_engine* e, int channels, int sample_rate, int frame_size = 0) : bn(make_unique<ma_phonon_binaural_node>()), audio_node_impl(nullptr, e) {
			if (!e) throw std::invalid_argument("no engine provided");
			if (!phonon_init()) throw std::runtime_error("Steam Audio was not initialized");
			if (!frame_size) frame_size = SOUNDSYSTEM_FRAMESIZE;
			IPLAudioSettings audio_settings {sample_rate, frame_size};
			ma_phonon_binaural_node_config cfg = ma_phonon_binaural_node_config_init(channels, audio_settings, g_phonon_context, g_phonon_hrtf);
			if ((g_soundsystem_last_error = ma_phonon_binaural_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*bn)) != MA_SUCCESS) throw std::runtime_error("phonon_binaural_node was not created");
			node = (ma_node_base*)&*bn;
		}
		~phonon_binaural_node_impl() {
			if (bn) ma_phonon_binaural_node_uninit(&*bn, nullptr);
		}
		void set_direction(float x, float y, float z, float distance) override { ma_phonon_binaural_node_set_direction(&*bn, x, y, z, distance); }
		void set_direction_vector(const reactphysics3d::Vector3& direction, float distance) override { ma_phonon_binaural_node_set_direction(&*bn, direction.x, direction.y, direction.z, distance); }
		void set_spatial_blend_max_distance(float max_distance) override { ma_phonon_binaural_node_set_spatial_blend_max_distance(&*bn, max_distance); }
};
phonon_binaural_node* phonon_binaural_node::create(audio_engine* e, int channels, int sample_rate, int frame_size) { return new phonon_binaural_node_impl(e, channels, sample_rate, frame_size); }

class splitter_node_impl : public audio_node_impl, public virtual splitter_node {
	unique_ptr<ma_splitter_node> sn;
	public:
	splitter_node_impl(audio_engine* e, int channels) : sn(make_unique<ma_splitter_node>()), audio_node_impl(nullptr, e) {
		ma_splitter_node_config cfg = ma_splitter_node_config_init(channels);
		if ((g_soundsystem_last_error = ma_splitter_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*sn)) != MA_SUCCESS) throw std::runtime_error("ma_splitter_node was not initialized");
		node = (ma_node_base*)&*sn;
	}
	~splitter_node_impl() {
		if (sn) ma_splitter_node_uninit(&*sn, nullptr);
	}
};
splitter_node* splitter_node::create(audio_engine* e, int channels) { return new splitter_node_impl(e, channels); }

class low_pass_filter_node_impl : public audio_node_impl, public virtual low_pass_filter_node {
	unique_ptr<ma_lpf_node> fn;
	ma_lpf_node_config cfg;
	public:
	low_pass_filter_node_impl(double cutoff_frequency, int order, audio_engine* e) : fn(make_unique<ma_lpf_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_lpf_node_config_init(e->get_channels(), e->get_sample_rate(), cutoff_frequency, order);
		if ((g_soundsystem_last_error = ma_lpf_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_low_pass_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~low_pass_filter_node_impl() {
		if (fn) ma_lpf_node_uninit(&*fn, nullptr);
	}
	void set_cutoff_frequency(double freq) override {
		cfg.lpf.cutoffFrequency = freq;
		ma_lpf_node_reinit(&cfg.lpf, &*fn);
	}
	double get_cutoff_frequency() const override { return cfg.lpf.cutoffFrequency; }
	void set_order(unsigned int order) override {
		cfg.lpf.order = order;
		ma_lpf_node_reinit(&cfg.lpf, &*fn);
	}
	unsigned int get_order() const override { return cfg.lpf.order; }
};
low_pass_filter_node* low_pass_filter_node::create(double cutoff_frequency, unsigned int order, audio_engine* engine) { return new low_pass_filter_node_impl(cutoff_frequency, order, engine); }

class high_pass_filter_node_impl : public audio_node_impl, public virtual high_pass_filter_node {
	unique_ptr<ma_hpf_node> fn;
	ma_hpf_node_config cfg;
	public:
	high_pass_filter_node_impl(double cutoff_frequency, int order, audio_engine* e) : fn(make_unique<ma_hpf_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_hpf_node_config_init(e->get_channels(), e->get_sample_rate(), cutoff_frequency, order);
		if ((g_soundsystem_last_error = ma_hpf_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_high_pass_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~high_pass_filter_node_impl() {
		if (fn) ma_hpf_node_uninit(&*fn, nullptr);
	}
	void set_cutoff_frequency(double freq) override {
		cfg.hpf.cutoffFrequency = freq;
		ma_hpf_node_reinit(&cfg.hpf, &*fn);
	}
	double get_cutoff_frequency() const override { return cfg.hpf.cutoffFrequency; }
	void set_order(unsigned int order) override {
		cfg.hpf.order = order;
		ma_hpf_node_reinit(&cfg.hpf, &*fn);
	}
	unsigned int get_order() const override { return cfg.hpf.order; }
};
high_pass_filter_node* high_pass_filter_node::create(double cutoff_frequency, unsigned int order, audio_engine* engine) { return new high_pass_filter_node_impl(cutoff_frequency, order, engine); }

class band_pass_filter_node_impl : public audio_node_impl, public virtual band_pass_filter_node {
	unique_ptr<ma_bpf_node> fn;
	ma_bpf_node_config cfg;
	public:
	band_pass_filter_node_impl(double cutoff_frequency, int order, audio_engine* e) : fn(make_unique<ma_bpf_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_bpf_node_config_init(e->get_channels(), e->get_sample_rate(), cutoff_frequency, order);
		if ((g_soundsystem_last_error = ma_bpf_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_band_pass_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~band_pass_filter_node_impl() {
		if (fn) ma_bpf_node_uninit(&*fn, nullptr);
	}
	void set_cutoff_frequency(double freq) override {
		cfg.bpf.cutoffFrequency = freq;
		ma_bpf_node_reinit(&cfg.bpf, &*fn);
	}
	double get_cutoff_frequency() const override { return cfg.bpf.cutoffFrequency; }
	void set_order(unsigned int order) override {
		cfg.bpf.order = order;
		ma_bpf_node_reinit(&cfg.bpf, &*fn);
	}
	unsigned int get_order() const override { return cfg.bpf.order; }
};
band_pass_filter_node* band_pass_filter_node::create(double cutoff_frequency, unsigned int order, audio_engine* engine) { return new band_pass_filter_node_impl(cutoff_frequency, order, engine); }

class notch_filter_node_impl : public audio_node_impl, public virtual notch_filter_node {
	unique_ptr<ma_notch_node> fn;
	ma_notch_node_config cfg;
	public:
	notch_filter_node_impl(double q, double frequency, audio_engine* e) : fn(make_unique<ma_notch_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_notch_node_config_init(e->get_channels(), e->get_sample_rate(), q, frequency);
		if ((g_soundsystem_last_error = ma_notch_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_notch_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~notch_filter_node_impl() {
		if (fn) ma_notch_node_uninit(&*fn, nullptr);
	}
	void set_q(double q) override {
		cfg.notch.q = q;
		ma_notch_node_reinit(&cfg.notch, &*fn);
	}
	double get_q() const override { return cfg.notch.q; }
	void set_frequency(double freq) override {
		cfg.notch.frequency = freq;
		ma_notch_node_reinit(&cfg.notch, &*fn);
	}
	double get_frequency() const override { return cfg.notch.frequency; }
};
notch_filter_node* notch_filter_node::create(double q, double frequency, audio_engine* engine) { return new notch_filter_node_impl(q, frequency, engine); }

class peak_filter_node_impl : public audio_node_impl, public virtual peak_filter_node {
	unique_ptr<ma_peak_node> fn;
	ma_peak_node_config cfg;
	public:
	peak_filter_node_impl(double gain_db, double q, double frequency, audio_engine* e) : fn(make_unique<ma_peak_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_peak_node_config_init(e->get_channels(), e->get_sample_rate(), gain_db, q, frequency);
		if ((g_soundsystem_last_error = ma_peak_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_peak_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~peak_filter_node_impl() {
		if (fn) ma_peak_node_uninit(&*fn, nullptr);
	}
	void set_gain(double gain) override {
		cfg.peak.gainDB = gain;
		ma_peak_node_reinit(&cfg.peak, &*fn);
	}
	double get_gain() const override { return cfg.peak.gainDB; }
	void set_q(double q) override {
		cfg.peak.q = q;
		ma_peak_node_reinit(&cfg.peak, &*fn);
	}
	double get_q() const override { return cfg.peak.q; }
	void set_frequency(double freq) override {
		cfg.peak.frequency = freq;
		ma_peak_node_reinit(&cfg.peak, &*fn);
	}
	double get_frequency() const override { return cfg.peak.frequency; }
};
peak_filter_node* peak_filter_node::create(double gain_db, double q, double frequency, audio_engine* engine) { return new peak_filter_node_impl(gain_db, q, frequency, engine); }

class low_shelf_filter_node_impl : public audio_node_impl, public virtual low_shelf_filter_node {
	unique_ptr<ma_loshelf_node> fn;
	ma_loshelf_node_config cfg;
	public:
	low_shelf_filter_node_impl(double gain_db, double q, double frequency, audio_engine* e) : fn(make_unique<ma_loshelf_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_loshelf_node_config_init(e->get_channels(), e->get_sample_rate(), gain_db, q, frequency);
		if ((g_soundsystem_last_error = ma_loshelf_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_low_shelf_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~low_shelf_filter_node_impl() {
		if (fn) ma_loshelf_node_uninit(&*fn, nullptr);
	}
	void set_gain(double gain) override {
		cfg.loshelf.gainDB = gain;
		ma_loshelf_node_reinit(&cfg.loshelf, &*fn);
	}
	double get_gain() const override { return cfg.loshelf.gainDB; }
	void set_q(double q) override {
		cfg.loshelf.shelfSlope = q;
		ma_loshelf_node_reinit(&cfg.loshelf, &*fn);
	}
	double get_q() const override { return cfg.loshelf.shelfSlope; }
	void set_frequency(double freq) override {
		cfg.loshelf.frequency = freq;
		ma_loshelf_node_reinit(&cfg.loshelf, &*fn);
	}
	double get_frequency() const override { return cfg.loshelf.frequency; }
};
low_shelf_filter_node* low_shelf_filter_node::create(double gain_db, double q, double frequency, audio_engine* engine) { return new low_shelf_filter_node_impl(gain_db, q, frequency, engine); }

class high_shelf_filter_node_impl : public audio_node_impl, public virtual high_shelf_filter_node {
	unique_ptr<ma_hishelf_node> fn;
	ma_hishelf_node_config cfg;
	public:
	high_shelf_filter_node_impl(double gain_db, double q, double frequency, audio_engine* e) : fn(make_unique<ma_hishelf_node>()), audio_node_impl(nullptr, e) {
		cfg = ma_hishelf_node_config_init(e->get_channels(), e->get_sample_rate(), gain_db, q, frequency);
		if ((g_soundsystem_last_error = ma_hishelf_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*fn)) != MA_SUCCESS) throw std::runtime_error("ma_high_shelf_filter_node was not initialized");
		node = (ma_node_base*)&*fn;
	}
	~high_shelf_filter_node_impl() {
		if (fn) ma_hishelf_node_uninit(&*fn, nullptr);
	}
	void set_gain(double gain) override {
		cfg.hishelf.gainDB = gain;
		ma_hishelf_node_reinit(&cfg.hishelf, &*fn);
	}
	double get_gain() const override { return cfg.hishelf.gainDB; }
	void set_q(double q) override {
		cfg.hishelf.shelfSlope = q;
		ma_hishelf_node_reinit(&cfg.hishelf, &*fn);
	}
	double get_q() const override { return cfg.hishelf.shelfSlope; }
	void set_frequency(double freq) override {
		cfg.hishelf.frequency = freq;
		ma_hishelf_node_reinit(&cfg.hishelf, &*fn);
	}
	double get_frequency() const override { return cfg.hishelf.frequency; }
};
high_shelf_filter_node* high_shelf_filter_node::create(double gain_db, double q, double frequency, audio_engine* engine) { return new high_shelf_filter_node_impl(gain_db, q, frequency, engine); }

class delay_node_impl : public audio_node_impl, public virtual delay_node {
	unique_ptr<ma_delay_node> dn;
	public:
	delay_node_impl(unsigned int delay_in_frames, float decay, audio_engine* e) : dn(make_unique<ma_delay_node>()), audio_node_impl(nullptr, e) {
		ma_delay_node_config cfg = ma_delay_node_config_init(e->get_channels(), e->get_sample_rate(), delay_in_frames, decay);
		if ((g_soundsystem_last_error = ma_delay_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*dn)) != MA_SUCCESS) throw std::runtime_error("ma_delay_node was not initialized");
		node = (ma_node_base*)&*dn;
	}
	~delay_node_impl() {
		if (dn) ma_delay_node_uninit(&*dn, nullptr);
	}
	void set_wet(float wet) override { ma_delay_node_set_wet(&*dn, wet); }
	float get_wet() const override { return ma_delay_node_get_wet(&*dn); }
	void set_dry(float dry) override { ma_delay_node_set_dry(&*dn, dry); }
	float get_dry() const override { return ma_delay_node_get_dry(&*dn); }
	void set_decay(float decay) override { ma_delay_node_set_decay(&*dn, decay); }
	float get_decay() const override { return ma_delay_node_get_decay(&*dn); }
};
delay_node* delay_node::create(unsigned int delay_in_frames, float decay, audio_engine* engine) { return new delay_node_impl(delay_in_frames, decay, engine); }

class freeverb_node_impl : public audio_node_impl, public virtual freeverb_node {
	unique_ptr<ma_reverb_node> rn;
	public:
	freeverb_node_impl(audio_engine* e) : rn(make_unique<ma_reverb_node>()), audio_node_impl(nullptr, e) {
		ma_reverb_node_config cfg = ma_reverb_node_config_init(e->get_channels(), e->get_sample_rate());
		if ((g_soundsystem_last_error = ma_reverb_node_init(ma_engine_get_node_graph(e->get_ma_engine()), &cfg, nullptr, &*rn)) != MA_SUCCESS) throw std::runtime_error("ma_reverb_node was not initialized");
		node = (ma_node_base*)&*rn;
	}
	~freeverb_node_impl() {
		if (rn) ma_reverb_node_uninit(&*rn, nullptr);
	}
	void set_room_size(float size) override { if (rn) verblib_set_room_size(&rn->reverb, size); }
	float get_room_size() const override { return rn? verblib_get_room_size(&rn->reverb) : -1; }
	void set_damping(float damping) override { if (rn) verblib_set_damping(&rn->reverb, damping); }
	float get_damping() const override { return rn? verblib_get_damping(&rn->reverb) : -1; }
	void set_width(float width) override { if (rn) verblib_set_width(&rn->reverb, width); }
	float get_width() const override { return rn? verblib_get_width(&rn->reverb) : -1; }
	void set_wet(float wet) override { if (rn) verblib_set_wet(&rn->reverb, wet); }
	float get_wet() const override { return rn? verblib_get_wet(&rn->reverb) : -1; }
	void set_dry(float dry) override { if (rn) verblib_set_dry(&rn->reverb, dry); }
	float get_dry() const override { return rn? verblib_get_dry(&rn->reverb) : -1; }
	void set_input_width(float width) override { if (rn) verblib_set_input_width(&rn->reverb, width); }
	float get_input_width() const override { return rn? verblib_get_input_width(&rn->reverb) : -1; }
	void set_frozen(bool frozen) override { if (rn) verblib_set_mode(&rn->reverb, frozen? 1 : 0); }
	bool get_frozen() const override { return rn? verblib_get_mode(&rn->reverb) >= 0.5 : false; }
};
freeverb_node* freeverb_node::create(audio_engine* e) { return new freeverb_node_impl(e); }

class reverb3d_impl : public passthrough_node_impl, public virtual reverb3d {
	audio_node* reverb;
	mixer* output_mixer;
	float min_volume, max_volume, max_volume_distance, max_audible_distance, volume_curve;
public:
	reverb3d_impl(audio_engine* e, audio_node* reverb, mixer* destination) : passthrough_node_impl(e), output_mixer(destination), reverb(reverb), min_volume(-7), max_volume(-5), max_volume_distance(7), max_audible_distance(60), volume_curve(0.4) {
		if (output_mixer) output_mixer->duplicate(); // reverb owns its own reference to output_mixer via attach_output_bus below; this is reverb3d_impl's separate reference for get_mixer().
		if (reverb) {
			attach_output_bus(0, reverb, 0); // reverb is owned automatically via audio_node_impl's output_connections tracking.
			if (output_mixer) reverb->attach_output_bus(0, output_mixer, 0);
			else reverb->attach_output_bus(0, e->get_endpoint(), 0);
		}
	}
	~reverb3d_impl() {
		if (output_mixer) output_mixer->release();
	}
	void set_reverb(audio_node* verb) override {
		if (reverb) {
			detach_output_bus(0);
			if (output_mixer) reverb->detach_output_bus(0);
		}
		reverb = verb;
		if (verb) {
			attach_output_bus(0, verb, 0);
			if (output_mixer) verb->attach_output_bus(0, output_mixer, 0);
		}
	}
	audio_node* get_reverb() const override { return reverb; }
	void set_mixer(mixer* mix) override {
		if (output_mixer) {
			if (reverb) reverb->detach_output_bus(0);
			output_mixer->release();
		}
		output_mixer = mix;
		if (output_mixer) output_mixer->duplicate();
		if (mix && reverb) reverb->attach_output_bus(0, mix, 0);
		else if (reverb) reverb->attach_output_bus(0, get_engine()->get_endpoint(), 0);
	}
	mixer* get_mixer() const override { return output_mixer; }
	void set_min_volume(float value) override { min_volume = value; }
	float get_min_volume() const override { return min_volume; }
	void set_max_volume(float value) override { max_volume = value; }
	float get_max_volume() const override { return max_volume; }
	void set_max_volume_distance(float value) override { max_volume_distance = value; }
	float get_max_volume_distance() const override { return max_volume_distance; }
	void set_max_audible_distance(float value) override { max_audible_distance = value; }
	float get_max_audible_distance() const override { return max_audible_distance; }
	void set_volume_curve(float value) override { volume_curve = value; }
	float get_volume_curve() const override { return volume_curve; }
	float get_volume_at(float distance) const override {
		if (distance > max_audible_distance) distance = max_audible_distance;
		float v;
		if (distance <= max_volume_distance) v = range_convert(distance, 0, max_volume_distance, min_volume, max_volume);
		else {
			if (volume_curve <= 0) return ma_volume_db_to_linear(max_volume);
			else if (volume_curve >= 1) return ma_volume_db_to_linear(min_volume);
			v = range_convert(distance, max_volume_distance, max_audible_distance, 1, 0);
			v = (1.0 - volume_curve) * ((v <= volume_curve? v : volume_curve) / volume_curve) + volume_curve * ((v > volume_curve? v - volume_curve : 0) / (1 - volume_curve));
			v = range_convert(v, 0, 1, -60, max_volume);
		}
		return ma_volume_db_to_linear(clamp(v, -70.0f, max_volume));
	}
	splitter_node* create_attachment(audio_node* dry_input, audio_node* dry_output) override {
		splitter_node* splitter = splitter_node::create(get_engine(), get_output_channels(0));
		if (dry_output && !splitter->attach_output_bus(0, dry_output, 0)) {
			splitter->release();
			return nullptr;
		}
		if (!splitter->attach_output_bus(1, this, 0)) {
			splitter->release();
			return nullptr;
		}
		if (dry_input && !dry_input->attach_output_bus(0, splitter, 0)) {
			splitter->release();
			return nullptr;
		}
		return splitter;
	}
};
reverb3d* reverb3d::create(audio_node* reverb, mixer* destination, audio_engine* e) { return new reverb3d_impl(e, reverb, destination); }

typedef struct {
	ma_node_base base;
	plugin_node* impl;
} ma_plugin_node;
static void ma_plugin_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut) {
	ma_plugin_node* n = (ma_plugin_node*)pNode;
	if (n->impl) n->impl->process(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
}
class plugin_node_impl : public audio_node_impl, public virtual plugin_node {
	unique_ptr<ma_plugin_node> pn;
	audio_plugin_node_interface* impl;
	ma_node_vtable vtable; // The user should be able to configure custom settings.
	public:
	plugin_node_impl(audio_plugin_node_interface* impl, unsigned char input_bus_count, unsigned char output_bus_count, unsigned int flags, audio_engine* engine) : pn(make_unique<ma_plugin_node>()), audio_node_impl(nullptr, engine), impl(impl) {
		vtable = { ma_plugin_node_process_pcm_frames, nullptr, input_bus_count, output_bus_count, flags };
		ma_node_config cfg = ma_node_config_init();
		ma_uint32 channels = engine->get_channels();
		cfg.vtable          = &vtable;
		cfg.pInputChannels  = &channels;
		cfg.pOutputChannels = &channels;
		if ((g_soundsystem_last_error = ma_node_init(ma_engine_get_node_graph(engine->get_ma_engine()), &cfg, nullptr, (ma_node_base*)&*pn)) != MA_SUCCESS) throw std::runtime_error("failed to create plugin_node");
		node = (ma_node_base*)&*pn;
	}
	~plugin_node_impl() {
		if (pn) ma_node_uninit(&*pn, nullptr);
	}
	audio_plugin_node_interface* get_plugin_interface() const override { return impl; }
	virtual void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		if (impl) impl->process(impl, frames_in, frame_count_in, frames_out, frame_count_out);
	}
};
plugin_node* plugin_node::create(audio_plugin_node_interface* impl, unsigned char input_bus_count, unsigned char output_bus_count, unsigned int flags,audio_engine* engine) { return new plugin_node_impl(impl, input_bus_count, output_bus_count, flags, engine); }

static std::unordered_map<int, spatializer_component> g_audio_panners;
static std::unordered_map<int, spatializer_component> g_audio_attenuators;
static int g_next_panner_id = 0;
static int g_next_attenuator_id = 0;
static int g_default_3d_panner = -1;
static int g_default_3d_attenuator = -1;
static std::unordered_set<audio_spatializer*> g_tracked_spatializers;

int register_audio_panner(spatializer_component_node_factory factory, bool default_enabled) {
	int id = g_next_panner_id++;
	g_audio_panners[id] = {factory, default_enabled};
	return id;
}

int register_audio_attenuator(spatializer_component_node_factory factory, bool default_enabled) {
	int id = g_next_attenuator_id++;
	g_audio_attenuators[id] = {factory, default_enabled};
	return id;
}

spatializer_component_node* create_audio_panner(int id, audio_spatializer* spatializer, audio_engine* engine) {
	auto it = g_audio_panners.find(id);
	if (it == g_audio_panners.end() || !it->second.enabled) return nullptr;
	return it->second.factory(spatializer, engine);
}

spatializer_component_node* create_audio_attenuator(int id, audio_spatializer* spatializer, audio_engine* engine) {
	auto it = g_audio_attenuators.find(id);
	if (it == g_audio_attenuators.end() || !it->second.enabled) return nullptr;
	return it->second.factory(spatializer, engine);
}

void set_audio_panner_enabled(int id, bool enabled) {
	auto it = g_audio_panners.find(id);
	if (it != g_audio_panners.end()) {
		bool was_enabled = it->second.enabled;
		it->second.enabled = enabled;
		if (was_enabled != enabled) {
			for (auto* spatializer : g_tracked_spatializers) {
				if (spatializer) spatializer->on_panner_enabled_changed(id, enabled);
			}
		}
	}
}

void set_audio_attenuator_enabled(int id, bool enabled) {
	auto it = g_audio_attenuators.find(id);
	if (it != g_audio_attenuators.end()) {
		bool was_enabled = it->second.enabled;
		it->second.enabled = enabled;
		if (was_enabled != enabled) {
			for (auto* spatializer : g_tracked_spatializers) {
				if (spatializer) spatializer->on_attenuator_enabled_changed(id, enabled);
			}
		}
	}
}

bool get_audio_panner_enabled(int id) {
	auto it = g_audio_panners.find(id);
	return it != g_audio_panners.end() && it->second.enabled;
}

bool get_audio_attenuator_enabled(int id) {
	auto it = g_audio_attenuators.find(id);
	return it != g_audio_attenuators.end() && it->second.enabled;
}

void sound_set_default_3d_panner(int panner_id) {
	g_default_3d_panner = panner_id;
}

int sound_get_default_3d_panner() {
	return g_default_3d_panner;
}

void sound_set_default_3d_attenuator(int attenuator_id) {
	g_default_3d_attenuator = attenuator_id;
}

int sound_get_default_3d_attenuator() {
	return g_default_3d_attenuator;
}

// Global  spatializer component registrations
int g_audio_basic_panner = g_default_3d_panner = register_audio_panner(basic_panner::create, true);
int g_audio_phonon_hrtf_panner = register_audio_panner(phonon_hrtf_panner::create, false);
int g_audio_basic_attenuator = g_default_3d_attenuator = register_audio_attenuator(basic_attenuator::create, true);
int g_audio_phonon_attenuator = register_audio_attenuator(phonon_attenuator::create, false);

bool sound_set_spatialization(int panner, int attenuator, bool disable_previous, bool set_default) {
	bool panner_success = false, attenuator_success = false;
	int prev_panner = g_default_3d_panner, prev_attenuator = g_default_3d_attenuator;
	auto panner_it = g_audio_panners.find(panner);
	if (panner_it != g_audio_panners.end()) {
		if (!panner_it->second.enabled) set_audio_panner_enabled(panner, true);
		if (set_default) sound_set_default_3d_panner(panner);
		panner_success = true;
		if (panner != prev_panner && disable_previous) set_audio_panner_enabled(prev_panner, false);
	}
	auto attenuator_it = g_audio_attenuators.find(attenuator);
	if (attenuator_it != g_audio_attenuators.end()) {
		if (!attenuator_it->second.enabled) set_audio_attenuator_enabled(attenuator, true);
		if (set_default) sound_set_default_3d_attenuator(attenuator);
		attenuator_success = true;
		if (attenuator != prev_attenuator && disable_previous) set_audio_attenuator_enabled(prev_attenuator, false);
	}
	return panner_success && attenuator_success;
}

class audio_spatializer_impl : public audio_node_chain_impl, public virtual audio_spatializer {
private:
	spatializer_component_node* panner;
	spatializer_component_node* attenuator;
	reverb3d* reverb;
	splitter_node* reverb_attachment;
	audio_spatializer_reverb3d_placement reverb_placement;
	mixer* attached_mixer;
	audio_spatialization_parameters spatialization_params;
	bool parameters_valid;
	int preferred_panner_id, preferred_attenuator_id;
	int current_panner_id, current_attenuator_id;
	void position_reverb() {
		if (!reverb_attachment) return;
		remove_node(reverb_attachment);
		switch (reverb_placement) {
			case prepan:
				add_node(reverb_attachment, nullptr, 0);
				break;
			case postpan:
				if (panner) add_node(reverb_attachment, panner, 0);
				else add_node(reverb_attachment, nullptr, 0);
				break;
			case postattenuate:
				if (attenuator) add_node(reverb_attachment, attenuator, 0);
				else if (panner) add_node(reverb_attachment, panner, 0);
				else add_node(reverb_attachment, nullptr, 0);
				break;
		}
	}
	bool set_fallback_panner() {
		for (const auto& pair : g_audio_panners) {
			if (!pair.second.enabled) continue;
			spatializer_component_node* fallback_panner = create_audio_panner(pair.first, this, get_engine());
			if (!fallback_panner) continue;
			current_panner_id = pair.first;
			set_panner(fallback_panner);
			return true;
		}
		return false;
	}
	bool set_fallback_attenuator() {
		for (const auto& pair : g_audio_attenuators) {
			if (!pair.second.enabled) continue;
			spatializer_component_node* fallback_attenuator = create_audio_attenuator(pair.first, this, get_engine());
			if (!fallback_attenuator) continue;
			current_attenuator_id = pair.first;
			set_attenuator(fallback_attenuator);
			return true;
		}
		return false;
	}
public:
	audio_spatializer_impl(mixer* mixer, audio_engine* engine) : audio_node_chain_impl(nullptr, nullptr, engine), panner(nullptr), attenuator(nullptr), reverb(nullptr), reverb_attachment(nullptr), reverb_placement(postpan), attached_mixer(mixer), spatialization_params{}, parameters_valid(true), preferred_panner_id(-1), preferred_attenuator_id(-1), current_panner_id(-1), current_attenuator_id(-1) {
		if (!mixer) throw std::invalid_argument("mixer cannot be null");
		spatialization_params.rolloff = 1.0f;
		spatialization_params.directional_attenuation_factor = 1.0f;
		g_tracked_spatializers.insert(this);
	}
	~audio_spatializer_impl() {
		destroy_node();
		g_tracked_spatializers.erase(this);
		if (panner) panner->release();
		if (attenuator) attenuator->release();
		if (reverb_attachment) reverb_attachment->release();
		if (reverb) reverb->release();
	}
	void set_panner(spatializer_component_node* new_panner) override {
		if (panner) {
			remove_node(panner);
			panner->release();
		}
		panner = new_panner;
		if (panner) {
			if (reverb_attachment && reverb_placement == prepan) add_node(panner, reverb_attachment, 0);
			else add_node(panner, nullptr, 0);
		}
	}
	void set_attenuator(spatializer_component_node* new_attenuator) override {
		if (attenuator) {
			remove_node(attenuator);
			attenuator->release();
		}
		attenuator = new_attenuator;
		if (attenuator) {
			if (panner && (!reverb_attachment || reverb_placement != postpan)) add_node(attenuator, panner, 0);
			else if (reverb_attachment && reverb_placement == postpan) add_node(attenuator, reverb_attachment, 0);
			else if (!panner && reverb_attachment && reverb_placement == prepan) add_node(attenuator, reverb_attachment, 0);
			else add_node(attenuator, nullptr, 0);
		}
	}
	void set_reverb3d(reverb3d* new_reverb, audio_spatializer_reverb3d_placement placement = postpan) override {
		if (new_reverb == reverb) return;
		audio_node* tmp;
		if (reverb) {
			tmp = reverb;
			reverb = nullptr;
			tmp->release();
			if (reverb_attachment) {
				remove_node(reverb_attachment);
				tmp = reverb_attachment;
				reverb_attachment = nullptr;
				tmp->release();
			}
		}
		if (new_reverb) {
			reverb_attachment = new_reverb->create_attachment();
			if (!reverb_attachment) return;
			if (attached_mixer && parameters_valid) reverb_attachment->set_output_bus_volume(1, new_reverb->get_volume_at(spatialization_params.listener_distance));
			reverb_placement = placement;
			position_reverb();
			new_reverb->duplicate();
		}
		reverb = new_reverb;
	}
	spatializer_component_node* get_panner() const override { return panner; }
	spatializer_component_node* get_attenuator() const override { return attenuator; }
	reverb3d* get_reverb3d() const override { return reverb; }
	splitter_node* get_reverb3d_attachment() const override { return reverb_attachment; }
	audio_spatializer_reverb3d_placement get_reverb3d_placement() const override { return reverb_placement; }
	mixer* get_mixer() const override { return attached_mixer; }
	void set_panner_by_id(int panner_id) override {
		preferred_panner_id = panner_id;
		spatializer_component_node* new_panner = create_audio_panner(panner_id, this, get_engine());
		if (new_panner) {
			current_panner_id = panner_id;
			set_panner(new_panner);
		} else if (!set_fallback_panner()) {
			current_panner_id = -1;
			set_panner(nullptr);
		}
	}
	void set_attenuator_by_id(int attenuator_id) override {
		preferred_attenuator_id = attenuator_id;
		spatializer_component_node* new_attenuator = create_audio_attenuator(attenuator_id, this, get_engine());
		if (new_attenuator) {
			current_attenuator_id = attenuator_id;
			set_attenuator(new_attenuator);
		} else if (!set_fallback_attenuator()) {
			current_attenuator_id = -1;
			set_attenuator(nullptr);
		}
	}
	int get_current_panner_id() const override { return current_panner_id; }
	int get_current_attenuator_id() const override { return current_attenuator_id; }
	int get_preferred_panner_id() const override { return preferred_panner_id; }
	int get_preferred_attenuator_id() const override { return preferred_attenuator_id; }
	void set_rolloff(float rolloff) override { spatialization_params.rolloff = clamp(rolloff, 0.0f, 100.0f); }
	float get_rolloff() const override { return spatialization_params.rolloff; }
	void set_directional_attenuation_factor(float factor) override { spatialization_params.directional_attenuation_factor = clamp(factor, 0.0f, 100.0f); }
	float get_directional_attenuation_factor() const override { return spatialization_params.directional_attenuation_factor; }
	bool get_parameters(audio_spatialization_parameters& params) override {
		if (!parameters_valid) return false;
		params = spatialization_params;
		return true;
	}
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		if (attached_mixer && (panner || attenuator || reverb)) {
			parameters_valid = attached_mixer->get_spatialization_parameters(spatialization_params);
			if (parameters_valid && reverb) reverb_attachment->set_output_bus_volume(1, reverb->get_volume_at(spatialization_params.listener_distance));
		} else parameters_valid = false;
	}
	void on_panner_enabled_changed(int panner_id, bool enabled) override {
		if (enabled) {
			if (preferred_panner_id == panner_id && preferred_panner_id != current_panner_id) {
				spatializer_component_node* new_panner = create_audio_panner(panner_id, this, get_engine());
				if (new_panner) {
					current_panner_id = panner_id;
					set_panner(new_panner);
				}
			}
		} else {
			if (current_panner_id == panner_id && !set_fallback_panner()) {
				current_panner_id = -1;
				set_panner(nullptr);
			}
		}
	}
	void on_attenuator_enabled_changed(int attenuator_id, bool enabled) override {
		if (enabled) {
			if (preferred_attenuator_id == attenuator_id && preferred_attenuator_id != current_attenuator_id) {
				spatializer_component_node* new_attenuator = create_audio_attenuator(attenuator_id, this, get_engine());
				if (new_attenuator) {
					current_attenuator_id = attenuator_id;
					set_attenuator(new_attenuator);
				}
			}
		} else {
			if (current_attenuator_id == attenuator_id && !set_fallback_attenuator()) {
				current_attenuator_id = -1;
				set_attenuator(nullptr);
			}
		}
	}
};
audio_spatializer* audio_spatializer::create(mixer* mixer, audio_engine* engine) { return new audio_spatializer_impl(mixer, engine); }

class spatializer_component_node_impl : public effect_node_impl, public virtual spatializer_component_node {
protected:
	audio_spatializer* spatializer;
public:
	spatializer_component_node_impl(audio_spatializer* spatializer, audio_engine* e, unsigned int input_channels = 0, unsigned int output_channels = 0, unsigned int input_bus_count = 1, unsigned int output_bus_count = 1, unsigned int flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING) : effect_node_impl(e, input_channels, output_channels, input_bus_count, output_bus_count, flags), spatializer(spatializer) {
		if (!spatializer) throw std::invalid_argument("spatializer cannot be null");
	}
	audio_spatializer* get_spatializer() const override { return spatializer; }
};

class basic_panner_impl : public spatializer_component_node_impl, public virtual basic_panner {
	ma_panner panner;
public:
	basic_panner_impl(audio_spatializer* spatializer, audio_engine* e) : spatializer_component_node_impl(spatializer, e, 0, 2) {
		ma_panner_config panner_config = ma_panner_config_init(ma_format_f32, 2);
		if (ma_panner_init(&panner_config, &panner) != MA_SUCCESS) throw std::runtime_error("Failed to initialize panner");
	}
	~basic_panner_impl() { destroy_node(); }
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		ma_uint32 frameCount = *frame_count_out;
		if (!spatializer) goto fail;
		audio_spatialization_parameters params;
		if (!spatializer->get_parameters(params)) goto fail;
		ma_panner_set_pan(&panner, pan_db_to_linear((params.listener_direction_x * params.listener_distance) * params.directional_attenuation_factor * 1.75));
		if (frameCount > *frame_count_in) frameCount = *frame_count_in;
		ma_panner_process_pcm_frames(&panner, frames_out[0], frames_in[0], frameCount);
		return;
		fail:
			ma_copy_pcm_frames(frames_out[0], frames_in[0], *frame_count_in, ma_format_f32, get_engine()->get_channels());
	}
};
spatializer_component_node* basic_panner::create(audio_spatializer* spatializer, audio_engine* engine) { return new basic_panner_impl(spatializer, engine); }

class phonon_hrtf_panner_impl : public spatializer_component_node_impl, public virtual phonon_hrtf_panner {
	IPLBinauralEffect iplEffect;
	IPLBinauralEffectParams iplEffectParams;
	IPLAudioBuffer inputBuffer;
	IPLAudioBuffer outputBuffer;
public:
	phonon_hrtf_panner_impl(audio_spatializer* spatializer, audio_engine* e) : spatializer_component_node_impl(spatializer, e, 0, 2), iplEffect(nullptr) {
		if (!phonon_init()) throw std::runtime_error("Steam Audio initialization failed");
		IPLBinauralEffectSettings effectSettings{};
		effectSettings.hrtf = g_phonon_hrtf;
		iplEffectParams = {};
		iplEffectParams.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
		iplEffectParams.spatialBlend = 1.0;
		iplEffectParams.hrtf = g_phonon_hrtf;
		if (iplBinauralEffectCreate(g_phonon_context, &g_phonon_audio_settings, &effectSettings, &iplEffect) != IPL_STATUS_SUCCESS) throw std::runtime_error("Failed to create binaural effect");
		ma_uint32 channelsIn = e->get_channels();
		if (iplAudioBufferAllocate(g_phonon_context, channelsIn, g_phonon_audio_settings.frameSize, &inputBuffer) != IPL_STATUS_SUCCESS) {
			iplBinauralEffectRelease(&iplEffect);
			throw std::runtime_error("Failed to allocate input audio buffer");
		}
		if (iplAudioBufferAllocate(g_phonon_context, 2, g_phonon_audio_settings.frameSize, &outputBuffer) != IPL_STATUS_SUCCESS) {
			iplAudioBufferFree(g_phonon_context, &inputBuffer);
			iplBinauralEffectRelease(&iplEffect);
			throw std::runtime_error("Failed to allocate output audio buffer");
		}
	}
	~phonon_hrtf_panner_impl() {	
		destroy_node();
		if (iplEffect) iplBinauralEffectRelease(&iplEffect);
		iplAudioBufferFree(g_phonon_context, &inputBuffer);
		iplAudioBufferFree(g_phonon_context, &outputBuffer);
	}
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		ma_uint32 totalFramesToProcess = *frame_count_out;
		ma_uint32 totalFramesProcessed = 0;
		float fully_spatialized_distance = 5; // Make this configurable once spatializer components have a property system.
		if (fully_spatialized_distance < 0.1) fully_spatialized_distance = 0.1;
		if (!spatializer || !iplEffect) goto fail;
		audio_spatialization_parameters params;
		if (!spatializer->get_parameters(params)) goto fail;
		iplEffectParams.direction.x = params.listener_direction_x * params.directional_attenuation_factor;
		iplEffectParams.direction.y = params.listener_direction_y * params.directional_attenuation_factor;
		iplEffectParams.direction.z = params.listener_direction_z * params.directional_attenuation_factor;
		iplEffectParams.spatialBlend = clamp(params.listener_distance * (params.directional_attenuation_factor / fully_spatialized_distance), 0.0f, 1.0f);
		while (totalFramesProcessed < totalFramesToProcess) {
			ma_uint32 framesToProcessThisIteration = totalFramesToProcess - totalFramesProcessed;
			if (framesToProcessThisIteration > (ma_uint32)g_phonon_audio_settings.frameSize) framesToProcessThisIteration = (ma_uint32)g_phonon_audio_settings.frameSize;
			inputBuffer.numSamples = framesToProcessThisIteration;
			outputBuffer.numSamples = framesToProcessThisIteration;
			iplAudioBufferDeinterleave(g_phonon_context, (float*)ma_offset_pcm_frames_const_ptr_f32(frames_in[0], totalFramesProcessed, inputBuffer.numChannels), &inputBuffer);
			iplBinauralEffectApply(iplEffect, &iplEffectParams, &inputBuffer, &outputBuffer);
			iplAudioBufferInterleave(g_phonon_context, &outputBuffer, ma_offset_pcm_frames_ptr_f32(frames_out[0], totalFramesProcessed, 2));
			totalFramesProcessed += framesToProcessThisIteration;
		}
		return;
		fail:
			ma_copy_pcm_frames(frames_out[0], frames_in[0], *frame_count_in, ma_format_f32, get_engine()->get_channels());
	}
};
spatializer_component_node* phonon_hrtf_panner::create(audio_spatializer* spatializer, audio_engine* engine) { return new phonon_hrtf_panner_impl(spatializer, engine); }

class basic_attenuator_impl : public spatializer_component_node_impl, public virtual basic_attenuator {
	ma_gainer gainer;
public:
	basic_attenuator_impl(audio_spatializer* spatializer, audio_engine* e) : spatializer_component_node_impl(spatializer, e) {
		ma_gainer_config gainer_config = ma_gainer_config_init(e->get_channels(), 1.0f);
		if (ma_gainer_init(&gainer_config, nullptr, &gainer) != MA_SUCCESS) {
			throw std::runtime_error("Failed to initialize gainer");
		}
	}
	~basic_attenuator_impl() {
		destroy_node();
		ma_gainer_uninit(&gainer, nullptr);
	}
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		float distance = 0, volume = 0;
		ma_uint32 frameCount = *frame_count_out;
		if (!spatializer) goto fail;
		audio_spatialization_parameters params;
		if (!spatializer->get_parameters(params)) goto fail;
		distance = params.listener_distance;
		if (distance >= params.min_distance) distance -= params.min_distance;
		volume = clamp(distance <= params.max_distance - params.min_distance? ma_volume_db_to_linear(-distance * params.rolloff * 1.75) : 0, params.min_volume, params.max_volume);
		ma_gainer_set_master_volume(&gainer, volume);
		if (frameCount > *frame_count_in) frameCount = *frame_count_in;
		ma_gainer_process_pcm_frames(&gainer, frames_out[0], frames_in[0], frameCount);
		return;
		fail:
			ma_copy_pcm_frames(frames_out[0], frames_in[0], *frame_count_in, ma_format_f32, get_engine()->get_channels());
	}
};
spatializer_component_node* basic_attenuator::create(audio_spatializer* spatializer, audio_engine* engine) { return new basic_attenuator_impl(spatializer, engine); }

class phonon_attenuator_impl : public spatializer_component_node_impl, public virtual phonon_attenuator {
	IPLDirectEffect iplEffect;
	IPLDirectEffectParams iplEffectParams;
	IPLAudioBuffer inputBuffer;
	IPLAudioBuffer outputBuffer;
	IPLDistanceAttenuationModel distanceModel;
	IPLAirAbsorptionModel airAbsorptionModel;
public:
	// Occlusion from a sound_environment, eased towards the simulated values a little each block so changes (a door closing) don't click.
	float current_occlusion = 1.0f;
	float current_transmission[3] = {1.0f, 1.0f, 1.0f};
	bool environment_started = false;
	static constexpr IPLDirectEffectFlags base_flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
	// Returns false if this block should be silent: a new sound in an environment waits for its first simulation, rather than blurting out unoccluded.
	bool apply_environment(const audio_spatialization_parameters& params) {
		iplEffectParams.flags = base_flags;
		mixer* mix = spatializer ? spatializer->get_mixer() : nullptr;
		sound_environment_source* source = mix ? mix->get_environment_source() : nullptr;
		if (!source) {
			environment_started = false;
			return true;
		}
		// Positions in the environment's world: the sound's and listener's own overrides if set (sound_pool positions sounds relative to the listener), else where the engine has them.
		float sx = params.sound_x, sy = params.sound_y, sz = params.sound_z, lx = params.listener_x, ly = params.listener_y, lz = params.listener_z;
		mix->get_occlusion_position(sx, sy, sz);
		sound_environment* env = mix->get_effective_environment();
		if (env) env->get_listener(lx, ly, lz);
		sound_environment_source_set_positions(source, sx, sy, sz, lx, ly, lz);
		if (!sound_environment_source_ready(source) && !sound_environment_try_prefill(env, source)) return false;
		float occlusion, transmission[3], route[3];
		sound_environment_source_get_results(source, occlusion, transmission);
		sound_environment_source_get_route_bands(source, route);
		if (route[1] > 0) {
			// Steam Audio gives occlusion + (1 - occlusion) * transmission per band. Fold the route in as transmission with occlusion 0.
			for (int i = 0; i < 3; i++) {
				float direct = occlusion + (1.0f - occlusion) * transmission[i];
				transmission[i] = std::min(1.0f, std::max(direct, route[i]));
			}
			occlusion = 0;
		}
		if (!environment_started) {
			current_occlusion = occlusion;
			for (int i = 0; i < 3; i++) current_transmission[i] = transmission[i];
			environment_started = true;
		} else {
			const float ease = 0.25f;
			current_occlusion += (occlusion - current_occlusion) * ease;
			for (int i = 0; i < 3; i++) current_transmission[i] += (transmission[i] - current_transmission[i]) * ease;
		}
		iplEffectParams.flags = static_cast<IPLDirectEffectFlags>(base_flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
		iplEffectParams.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
		iplEffectParams.occlusion = current_occlusion;
		for (int i = 0; i < 3; i++) iplEffectParams.transmission[i] = current_transmission[i];
		return true;
	}
	phonon_attenuator_impl(audio_spatializer* spatializer, audio_engine* e) : spatializer_component_node_impl(spatializer, e), iplEffect(nullptr) {
		if (!phonon_init()) throw std::runtime_error("Steam Audio initialization failed");
		distanceModel = {};
		distanceModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
		distanceModel.minDistance = 1.0f;
		airAbsorptionModel = {};
		airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
		IPLDirectEffectSettings effectSettings{};
		effectSettings.numChannels = e->get_channels();
		iplEffectParams = {};
		iplEffectParams.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
		iplEffectParams.directivity = 1.0f;
		if (iplDirectEffectCreate(g_phonon_context, &g_phonon_audio_settings, &effectSettings, &iplEffect) != IPL_STATUS_SUCCESS) throw std::runtime_error("Failed to create direct effect");
		ma_uint32 channelsIn = e->get_channels();
		if (iplAudioBufferAllocate(g_phonon_context, channelsIn, g_phonon_audio_settings.frameSize, &inputBuffer) != IPL_STATUS_SUCCESS) {
			iplDirectEffectRelease(&iplEffect);
			throw std::runtime_error("Failed to allocate input audio buffer");
		}
		if (iplAudioBufferAllocate(g_phonon_context, channelsIn, g_phonon_audio_settings.frameSize, &outputBuffer) != IPL_STATUS_SUCCESS) {
			iplAudioBufferFree(g_phonon_context, &inputBuffer);
			iplDirectEffectRelease(&iplEffect);
			throw std::runtime_error("Failed to allocate output audio buffer");
		}
	}
	~phonon_attenuator_impl() {
		destroy_node();
		if (iplEffect) iplDirectEffectRelease(&iplEffect);
		iplAudioBufferFree(g_phonon_context, &inputBuffer);
		iplAudioBufferFree(g_phonon_context, &outputBuffer);
	}
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override {
		IPLVector3 sourcePos, listenerPos;
		ma_uint32 totalFramesToProcess = *frame_count_out;
		ma_uint32 totalFramesProcessed = 0;
		if (!spatializer || !iplEffect) goto fail;
		audio_spatialization_parameters params;
		if (!spatializer->get_parameters(params)) goto fail;
		distanceModel.minDistance = params.min_distance;
		distanceModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
		params.rolloff *= 0.7; // Attenuators apply internal factors sometimes to try making it so that rolloff at a constant value will cause a similar volume reduction regardless of the attenuator in use.
		sourcePos = {params.sound_x * params.rolloff, params.sound_y * params.rolloff, params.sound_z * params.rolloff};
		listenerPos = {params.listener_x * params.rolloff, params.listener_y * params.rolloff, params.listener_z * params.rolloff};
		iplEffectParams.distanceAttenuation = params.listener_distance <= params.max_distance? clamp(iplDistanceAttenuationCalculate(g_phonon_context, sourcePos, listenerPos, &distanceModel), params.min_volume, params.max_volume) : params.min_volume;
		iplAirAbsorptionCalculate(g_phonon_context, sourcePos, listenerPos, &airAbsorptionModel, iplEffectParams.airAbsorption);
		if (!apply_environment(params)) {
			ma_silence_pcm_frames(frames_out[0], totalFramesToProcess, ma_format_f32, get_engine()->get_channels());
			return;
		}
		while (totalFramesProcessed < totalFramesToProcess) {
			ma_uint32 framesToProcessThisIteration = totalFramesToProcess - totalFramesProcessed;
			if (framesToProcessThisIteration > (ma_uint32)g_phonon_audio_settings.frameSize) framesToProcessThisIteration = (ma_uint32)g_phonon_audio_settings.frameSize;
			inputBuffer.numSamples = framesToProcessThisIteration;
			outputBuffer.numSamples = framesToProcessThisIteration;
			iplAudioBufferDeinterleave(g_phonon_context, (float*)ma_offset_pcm_frames_const_ptr_f32(frames_in[0], totalFramesProcessed, inputBuffer.numChannels), &inputBuffer);
			iplDirectEffectApply(iplEffect, &iplEffectParams, &inputBuffer, &outputBuffer);
			iplAudioBufferInterleave(g_phonon_context, &outputBuffer, ma_offset_pcm_frames_ptr_f32(frames_out[0], totalFramesProcessed, outputBuffer.numChannels));
			totalFramesProcessed += framesToProcessThisIteration;
		}
		return;
		fail:
			ma_copy_pcm_frames(frames_out[0], frames_in[0], *frame_count_in, ma_format_f32, get_engine()->get_channels());
	}
};
spatializer_component_node* phonon_attenuator::create(audio_spatializer* spatializer, audio_engine* engine) { return new phonon_attenuator_impl(spatializer, engine); }

// sound_environment: occlusion and transmission for the current sound system (see sound_nodes.h).
// Added for Conversational on KamiKitsune420/nvgt (not upstream NVGT).

struct sound_environment_source {
	IPLSource source = nullptr;
	bool added = false; // Worker thread only.
	std::atomic<bool> active {true};
	std::atomic<bool> has_position {false};
	std::atomic<bool> has_results {false};
	std::atomic<float> sx {0}, sy {0}, sz {0}, lx {0}, ly {0}, lz {0};
	std::atomic<float> occlusion {1.0f};
	std::atomic<float> transmission[3] = {1.0f, 1.0f, 1.0f};
	// A route round through a portal (doorway), when that carries more sound than the straight line: how much gets there, how dull the bend
	// makes it (per band), where the portal is, and the total distance listener to portal to sound.
	std::atomic<bool> via_active {false};
	std::atomic<float> via_amount {0}, via_x {0}, via_y {0}, via_z {0}, via_distance {0};
	std::atomic<float> via_eq[3] = {1.0f, 1.0f, 1.0f};
};

void sound_environment_source_set_positions(sound_environment_source* s, float sound_x, float sound_y, float sound_z, float listener_x, float listener_y, float listener_z) {
	if (!s) return;
	s->sx.store(sound_x, std::memory_order_relaxed);
	s->sy.store(sound_y, std::memory_order_relaxed);
	s->sz.store(sound_z, std::memory_order_relaxed);
	s->lx.store(listener_x, std::memory_order_relaxed);
	s->ly.store(listener_y, std::memory_order_relaxed);
	s->lz.store(listener_z, std::memory_order_relaxed);
	s->has_position.store(true, std::memory_order_release);
}
bool sound_environment_source_ready(sound_environment_source* s) { return s && s->has_results.load(std::memory_order_acquire); }
bool sound_environment_source_get_route(sound_environment_source* s, float& x, float& y, float& z, float& distance) {
	if (!s || !s->via_active.load(std::memory_order_acquire)) return false;
	x = s->via_x.load();
	y = s->via_y.load();
	z = s->via_z.load();
	distance = s->via_distance.load();
	return true;
}
// For the attenuator: how much of each band the portal route carries (0 when there's no route).
void sound_environment_source_get_route_bands(sound_environment_source* s, float bands[3]) {
	bool active = s && s->via_active.load(std::memory_order_acquire);
	float amount = active ? s->via_amount.load() : 0.0f;
	for (int i = 0; i < 3; i++) bands[i] = active ? amount * s->via_eq[i].load() : 0.0f;
}
void sound_environment_source_get_results(sound_environment_source* s, float& occlusion, float transmission[3]) {
	occlusion = s->occlusion.load(std::memory_order_relaxed);
	for (int i = 0; i < 3; i++) transmission[i] = s->transmission[i].load(std::memory_order_relaxed);
}

class sound_environment_impl : public sound_environment {
	struct box {
		std::string material;
		float bounds[6];
		bool enabled = true;
		bool removed = false;
		IPLStaticMesh mesh = nullptr;
	};
	struct cached_result {
		float occlusion;
		float transmission[3];
		bool via_active;
		float via_amount, via_x, via_y, via_z, via_distance;
		float via_eq[3];
	};
	struct portal {
		float x, y, z, radius;
		bool enabled = true;
		bool removed = false;
		IPLSource source = nullptr;
		bool added = false;
		float visibility = 0; // From the listener, this pass.
	};
	std::vector<portal> portals;
	std::atomic<int> ref_count {1};
	IPLScene scene = nullptr;
	IPLSimulator sim = nullptr;
	std::unordered_map<std::string, IPLMaterial> materials;
	std::vector<box> boxes;
	std::vector<std::unique_ptr<sound_environment_source>> sources;
	// Recent results by (sound tile, listener tile), so a new sound where one just played (another footstep) starts with the right occlusion.
	std::unordered_map<uint64_t, cached_result> cache;
	mutable std::mutex m;
	std::condition_variable wake;
	std::thread worker;
	bool running = true;
	bool scene_dirty = false;
	bool sim_dirty = true;
	bool wake_requested = false;
	float occlusion_radius = 0.5f;
	int occlusion_samples = 16;
	int transmission_rays = 3;
	int update_rate = 20;
	static const int max_occlusion_samples = 64;

	static uint64_t cache_key(float sx, float sy, float sz, float lx, float ly, float lz) {
		int v[6] = {int(floorf(sx)), int(floorf(sy)), int(floorf(sz)), int(floorf(lx)), int(floorf(ly)), int(floorf(lz))};
		uint64_t h = 1469598103934665603ULL;
		for (int i = 0; i < 6; i++) {
			h ^= uint64_t(uint32_t(v[i]));
			h *= 1099511628211ULL;
		}
		return h;
	}
	bool make_mesh(box& b) {
		auto it = materials.find(b.material);
		if (it == materials.end()) return false;
		float minx = b.bounds[0], maxx = b.bounds[1], miny = b.bounds[2], maxy = b.bounds[3], minz = b.bounds[4], maxz = b.bounds[5];
		IPLVector3 vertices[8] = {{minx, miny, minz}, {maxx, miny, minz}, {maxx, maxy, minz}, {minx, maxy, minz}, {minx, miny, maxz}, {maxx, miny, maxz}, {maxx, maxy, maxz}, {minx, maxy, maxz}};
		IPLTriangle triangles[12] = {{0, 2, 1}, {0, 3, 2}, {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}, {4, 5, 6}, {4, 6, 7}};
		IPLint32 material_indices[12] = {0};
		IPLMaterial material = it->second;
		IPLStaticMeshSettings settings {8, 12, 1, vertices, triangles, material_indices, &material};
		if (iplStaticMeshCreate(scene, &settings, &b.mesh) != IPL_STATUS_SUCCESS) {
			b.mesh = nullptr;
			return false;
		}
		iplStaticMeshAdd(b.mesh, scene);
		scene_dirty = true;
		return true;
	}
	void drop_mesh(box& b) {
		if (!b.mesh) return;
		iplStaticMeshRemove(b.mesh, scene);
		iplStaticMeshRelease(&b.mesh);
		b.mesh = nullptr;
		scene_dirty = true;
	}
	void set_source_inputs(IPLSource source, float x, float y, float z, float radius) {
		IPLSimulationInputs inputs {};
		inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
		inputs.directFlags = IPLDirectSimulationFlags(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
		inputs.source = IPLCoordinateSpace3 {IPLVector3 {1, 0, 0}, IPLVector3 {0, 0, 1}, IPLVector3 {0, 1, 0}, IPLVector3 {x, y, z}};
		inputs.occlusionType = occlusion_samples > 1 ? IPL_OCCLUSIONTYPE_VOLUMETRIC : IPL_OCCLUSIONTYPE_RAYCAST;
		inputs.occlusionRadius = radius;
		inputs.numOcclusionSamples = occlusion_samples;
		inputs.numTransmissionRays = transmission_rays;
		iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
	}
	void run_from(float x, float y, float z) {
		IPLSimulationSharedInputs shared {};
		shared.listener = IPLCoordinateSpace3 {IPLVector3 {1, 0, 0}, IPLVector3 {0, 0, 1}, IPLVector3 {0, 1, 0}, IPLVector3 {x, y, z}};
		iplSimulatorSetSharedInputs(sim, IPL_SIMULATIONFLAGS_DIRECT, &shared);
		iplSimulatorRunDirect(sim);
	}
	static float read_occlusion(IPLSource source) {
		IPLSimulationOutputs outputs {};
		iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
		return outputs.direct.occlusion;
	}
	// How dull a route round a corner is, per band, from the bend at the portal (0 degrees straight on, 90 a right angle): low sound
	// bends round easily, high sound hardly at all.
	static void bend_eq(float degrees, float eq[3]) {
		float k = degrees / 90.0f;
		if (k < 0) k = 0;
		if (k > 2) k = 2;
		eq[0] = std::max(0.2f, 1.0f - 0.15f * k);
		eq[1] = std::max(0.08f, 1.0f - 0.45f * k);
		eq[2] = std::max(0.03f, 1.0f - 0.7f * k);
	}
	void run() {
		std::unique_lock<std::mutex> lock(m);
		while (running) {
			if (scene_dirty) {
				iplSceneCommit(scene);
				scene_dirty = false;
				sim_dirty = true;
				cache.clear(); // Walls changed (a door opened), so old results are wrong.
			}
			for (auto it = sources.begin(); it != sources.end();) {
				sound_environment_source* s = it->get();
				if (s->active.load()) {
					if (!s->added) {
						iplSourceAdd(s->source, sim);
						s->added = true;
						sim_dirty = true;
					}
					++it;
					continue;
				}
				if (s->added) iplSourceRemove(s->source, sim);
				iplSourceRelease(&s->source);
				it = sources.erase(it);
				sim_dirty = true;
			}
			for (portal& p : portals) {
				bool want = p.enabled && !p.removed;
				if (want && !p.source) {
					IPLSourceSettings settings {IPL_SIMULATIONFLAGS_DIRECT};
					if (iplSourceCreate(sim, &settings, &p.source) != IPL_STATUS_SUCCESS) p.source = nullptr;
				}
				if (want && p.source && !p.added) {
					iplSourceAdd(p.source, sim);
					p.added = true;
					sim_dirty = true;
				} else if (!want && p.added) {
					iplSourceRemove(p.source, sim);
					p.added = false;
					sim_dirty = true;
				}
			}
			if (sim_dirty) {
				iplSimulatorCommit(sim);
				sim_dirty = false;
			}
			bool have_listener = false;
			IPLVector3 listener {0, 0, 0};
			for (auto& src : sources) {
				if (!src->added || !src->has_position.load(std::memory_order_acquire)) continue;
				if (!have_listener) {
					listener = IPLVector3 {src->lx.load(), src->ly.load(), src->lz.load()};
					have_listener = true;
				}
				set_source_inputs(src->source, src->sx.load(), src->sy.load(), src->sz.load(), occlusion_radius);
			}
			for (portal& p : portals)
				if (p.added) set_source_inputs(p.source, p.x, p.y, p.z, p.radius);
			if (have_listener) {
				// 1: from the listener. Straight-line results for every sound, and which portals the listener can see.
				run_from(listener.x, listener.y, listener.z);
				std::vector<cached_result> results(sources.size());
				bool any_blocked = false;
				for (size_t i = 0; i < sources.size(); i++) {
					sound_environment_source* src = sources[i].get();
					if (!src->added || !src->has_position.load(std::memory_order_acquire)) continue;
					IPLSimulationOutputs outputs {};
					iplSourceGetOutputs(src->source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
					cached_result& r = results[i];
					r.occlusion = outputs.direct.occlusion;
					for (int b = 0; b < 3; b++) r.transmission[b] = outputs.direct.transmission[b];
					r.via_active = false;
					r.via_amount = 0;
					r.via_distance = 0;
					if (r.occlusion < 0.9f) any_blocked = true;
				}
				for (portal& p : portals) p.visibility = p.added ? read_occlusion(p.source) : 0.0f;
				// 2: from each portal the listener can see, for the sounds the straight line doesn't reach well.
				if (any_blocked) {
					for (portal& p : portals) {
						if (!p.added || p.visibility < 0.25f) continue;
						run_from(p.x, p.y, p.z);
						for (size_t i = 0; i < sources.size(); i++) {
							sound_environment_source* src = sources[i].get();
							cached_result& r = results[i];
							if (!src->added || !src->has_position.load(std::memory_order_acquire) || r.occlusion >= 0.9f) continue;
							float seen = read_occlusion(src->source);
							float amount = p.visibility * seen;
							if (amount < 0.05f) continue;
							float sx = src->sx.load(), sy = src->sy.load(), sz = src->sz.load();
							// The bend at the portal: between listener-to-portal and portal-to-sound.
							float ax = p.x - listener.x, ay = p.y - listener.y, az = p.z - listener.z;
							float bx = sx - p.x, by = sy - p.y, bz = sz - p.z;
							float la = sqrtf(ax * ax + ay * ay + az * az), lb = sqrtf(bx * bx + by * by + bz * bz);
							float degrees = 0;
							if (la > 0.001f && lb > 0.001f) {
								float c = (ax * bx + ay * by + az * bz) / (la * lb);
								c = c < -1 ? -1 : (c > 1 ? 1 : c);
								degrees = acosf(c) * 57.29578f;
							}
							float eq[3];
							bend_eq(degrees, eq);
							float route_distance = la + lb;
							// Pick the route that carries the most mid-band sound, shorter routes winning ties.
							float score = amount * eq[1] / (1.0f + route_distance * 0.05f);
							float best = r.via_amount > 0 ? r.via_amount * r.via_eq[1] / (1.0f + r.via_distance * 0.05f) : 0;
							if (score <= best) continue;
							r.via_amount = amount;
							for (int b = 0; b < 3; b++) r.via_eq[b] = eq[b];
							r.via_x = p.x;
							r.via_y = p.y;
							r.via_z = p.z;
							r.via_distance = route_distance;
						}
					}
				}
				// Publish. A route is used when it carries clearly more than the wall does, with some hysteresis so it doesn't flicker.
				for (size_t i = 0; i < sources.size(); i++) {
					sound_environment_source* src = sources[i].get();
					if (!src->added || !src->has_position.load(std::memory_order_acquire)) continue;
					cached_result& r = results[i];
					float direct_mid = r.occlusion + (1.0f - r.occlusion) * r.transmission[1];
					float via_mid = r.via_amount * r.via_eq[1];
					bool was_active = src->via_active.load();
					r.via_active = r.via_amount > 0 && via_mid > direct_mid * (was_active ? 0.8f : 1.25f);
					src->occlusion.store(r.occlusion, std::memory_order_relaxed);
					for (int b = 0; b < 3; b++) src->transmission[b].store(r.transmission[b], std::memory_order_relaxed);
					if (r.via_active) {
						src->via_amount.store(r.via_amount);
						for (int b = 0; b < 3; b++) src->via_eq[b].store(r.via_eq[b]);
						src->via_x.store(r.via_x);
						src->via_y.store(r.via_y);
						src->via_z.store(r.via_z);
						src->via_distance.store(r.via_distance);
					}
					src->via_active.store(r.via_active, std::memory_order_release);
					src->has_results.store(true, std::memory_order_release);
					if (cache.size() > 4096) cache.clear();
					cache[cache_key(src->sx.load(), src->sy.load(), src->sz.load(), src->lx.load(), src->ly.load(), src->lz.load())] = r;
				}
			}
			wake_requested = false;
			int rate = update_rate < 1 ? 1 : update_rate;
			wake.wait_for(lock, std::chrono::milliseconds(1000 / rate), [this] { return !running || wake_requested; });
		}
	}
public:
	sound_environment_impl() {
		if (!phonon_init()) throw std::runtime_error("sound_environment needs Steam Audio, which couldn't be started");
		IPLSceneSettings scene_settings {};
		scene_settings.type = IPL_SCENETYPE_DEFAULT;
		if (iplSceneCreate(g_phonon_context, &scene_settings, &scene) != IPL_STATUS_SUCCESS) throw std::runtime_error("couldn't create the sound_environment's scene");
		IPLSimulationSettings sim_settings {};
		sim_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
		sim_settings.sceneType = IPL_SCENETYPE_DEFAULT;
		sim_settings.maxNumOcclusionSamples = max_occlusion_samples;
		sim_settings.maxNumSources = 1024;
		sim_settings.numThreads = 1;
		sim_settings.samplingRate = g_phonon_audio_settings.samplingRate;
		sim_settings.frameSize = g_phonon_audio_settings.frameSize;
		if (iplSimulatorCreate(g_phonon_context, &sim_settings, &sim) != IPL_STATUS_SUCCESS) {
			iplSceneRelease(&scene);
			throw std::runtime_error("couldn't create the sound_environment's simulator");
		}
		iplSimulatorSetScene(sim, scene);
		iplSceneCommit(scene);
		iplSimulatorCommit(sim);
		// The legacy plugin's materials, so maps written for it sound the same.
		add_material("air", 0, 0, 0, 0, 1, 1, 1, false);
		add_material("generic", 0.10f, 0.20f, 0.30f, 0.05f, 0.100f, 0.050f, 0.030f, false);
		add_material("brick", 0.03f, 0.04f, 0.07f, 0.05f, 0.015f, 0.015f, 0.015f, false);
		add_material("concrete", 0.05f, 0.07f, 0.08f, 0.05f, 0.015f, 0.002f, 0.001f, false);
		add_material("ceramic", 0.01f, 0.02f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f, false);
		add_material("gravel", 0.60f, 0.70f, 0.80f, 0.05f, 0.031f, 0.012f, 0.008f, false);
		add_material("carpet", 0.24f, 0.69f, 0.73f, 0.05f, 0.020f, 0.005f, 0.003f, false);
		add_material("glass", 0.06f, 0.03f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f, false);
		add_material("plaster", 0.12f, 0.06f, 0.04f, 0.05f, 0.056f, 0.056f, 0.004f, false);
		add_material("wood", 0.11f, 0.07f, 0.06f, 0.05f, 0.070f, 0.014f, 0.005f, false);
		add_material("metal", 0.20f, 0.07f, 0.06f, 0.05f, 0.200f, 0.025f, 0.010f, false);
		add_material("rock", 0.13f, 0.20f, 0.24f, 0.05f, 0.015f, 0.002f, 0.001f, false);
		worker = std::thread(&sound_environment_impl::run, this);
	}
	~sound_environment_impl() {
		{
			std::lock_guard<std::mutex> lock(m);
			running = false;
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
		for (auto& s : sources) {
			if (s->added) iplSourceRemove(s->source, sim);
			iplSourceRelease(&s->source);
		}
		sources.clear();
		for (portal& p : portals) {
			if (!p.source) continue;
			if (p.added) iplSourceRemove(p.source, sim);
			iplSourceRelease(&p.source);
		}
		for (box& b : boxes) {
			if (!b.mesh) continue;
			iplStaticMeshRemove(b.mesh, scene);
			iplStaticMeshRelease(&b.mesh);
		}
		iplSimulatorRelease(&sim);
		iplSceneRelease(&scene);
	}
	void add_ref() override { ref_count.fetch_add(1); }
	void release() override {
		if (ref_count.fetch_sub(1) == 1) delete this;
	}
	bool add_material(const std::string& name, float absorption_low, float absorption_mid, float absorption_high, float scattering, float transmission_low, float transmission_mid, float transmission_high, bool replace_if_existing) override {
		std::lock_guard<std::mutex> lock(m);
		if (!replace_if_existing && materials.find(name) != materials.end()) return false;
		materials[name] = IPLMaterial {{absorption_low, absorption_mid, absorption_high}, scattering, {transmission_low, transmission_mid, transmission_high}};
		return true;
	}
	bool material_exists(const std::string& name) const override {
		std::lock_guard<std::mutex> lock(m);
		return materials.find(name) != materials.end();
	}
	int add_box(const std::string& material, float minx, float maxx, float miny, float maxy, float minz, float maxz, bool enabled) override {
		std::lock_guard<std::mutex> lock(m);
		if (materials.find(material) == materials.end()) return -1;
		box b;
		b.material = material;
		b.bounds[0] = std::min(minx, maxx);
		b.bounds[1] = std::max(minx, maxx);
		b.bounds[2] = std::min(miny, maxy);
		b.bounds[3] = std::max(miny, maxy);
		b.bounds[4] = std::min(minz, maxz);
		b.bounds[5] = std::max(minz, maxz);
		b.enabled = enabled;
		if (enabled && !make_mesh(b)) return -1;
		boxes.push_back(b);
		return int(boxes.size()) - 1;
	}
	bool remove_box(int id) override {
		std::lock_guard<std::mutex> lock(m);
		if (id < 0 || id >= int(boxes.size()) || boxes[id].removed) return false;
		drop_mesh(boxes[id]);
		boxes[id].removed = true;
		boxes[id].enabled = false;
		return true;
	}
	bool set_box_enabled(int id, bool enabled) override {
		std::lock_guard<std::mutex> lock(m);
		if (id < 0 || id >= int(boxes.size()) || boxes[id].removed) return false;
		box& b = boxes[id];
		if (b.enabled == enabled) return true;
		if (enabled) {
			if (!make_mesh(b)) return false;
		} else drop_mesh(b);
		b.enabled = enabled;
		wake_requested = true;
		wake.notify_all();
		return true;
	}
	bool get_box_enabled(int id) const override {
		std::lock_guard<std::mutex> lock(m);
		if (id < 0 || id >= int(boxes.size()) || boxes[id].removed) return false;
		return boxes[id].enabled;
	}
	void clear_boxes() override {
		std::lock_guard<std::mutex> lock(m);
		for (box& b : boxes) drop_mesh(b);
		boxes.clear();
		wake_requested = true;
		wake.notify_all();
	}
	unsigned int get_box_count() const override {
		std::lock_guard<std::mutex> lock(m);
		unsigned int count = 0;
		for (const box& b : boxes)
			if (!b.removed) count++;
		return count;
	}
	void set_occlusion_radius(float radius) override {
		std::lock_guard<std::mutex> lock(m);
		occlusion_radius = radius < 0 ? 0 : radius;
	}
	float get_occlusion_radius() const override {
		std::lock_guard<std::mutex> lock(m);
		return occlusion_radius;
	}
	void set_occlusion_samples(int samples) override {
		std::lock_guard<std::mutex> lock(m);
		occlusion_samples = samples < 1 ? 1 : (samples > max_occlusion_samples ? max_occlusion_samples : samples);
	}
	int get_occlusion_samples() const override {
		std::lock_guard<std::mutex> lock(m);
		return occlusion_samples;
	}
	void set_transmission_rays(int rays) override {
		std::lock_guard<std::mutex> lock(m);
		transmission_rays = rays < 1 ? 1 : rays;
	}
	int get_transmission_rays() const override {
		std::lock_guard<std::mutex> lock(m);
		return transmission_rays;
	}
	int add_portal(float x, float y, float z, float radius) override {
		std::lock_guard<std::mutex> lock(m);
		portal p;
		p.x = x;
		p.y = y;
		p.z = z;
		p.radius = radius < 0.1f ? 0.1f : radius;
		portals.push_back(p);
		cache.clear();
		wake_requested = true;
		wake.notify_all();
		return int(portals.size()) - 1;
	}
	bool remove_portal(int id) override {
		std::lock_guard<std::mutex> lock(m);
		if (id < 0 || id >= int(portals.size()) || portals[id].removed) return false;
		portals[id].removed = true;
		cache.clear();
		wake_requested = true;
		wake.notify_all();
		return true;
	}
	bool set_portal_enabled(int id, bool enabled) override {
		std::lock_guard<std::mutex> lock(m);
		if (id < 0 || id >= int(portals.size()) || portals[id].removed) return false;
		portals[id].enabled = enabled;
		cache.clear();
		wake_requested = true;
		wake.notify_all();
		return true;
	}
	bool get_portal_enabled(int id) const override {
		std::lock_guard<std::mutex> lock(m);
		return id >= 0 && id < int(portals.size()) && !portals[id].removed && portals[id].enabled;
	}
	unsigned int get_portal_count() const override {
		std::lock_guard<std::mutex> lock(m);
		unsigned int count = 0;
		for (const portal& p : portals)
			if (!p.removed) count++;
		return count;
	}
	std::atomic<bool> has_listener {false};
	std::atomic<float> listener_x {0}, listener_y {0}, listener_z {0};
	void set_listener(float x, float y, float z) override {
		listener_x.store(x);
		listener_y.store(y);
		listener_z.store(z);
		has_listener.store(true);
	}
	void clear_listener() override { has_listener.store(false); }
	bool get_listener(float& x, float& y, float& z) const override {
		if (!has_listener.load()) return false;
		x = listener_x.load();
		y = listener_y.load();
		z = listener_z.load();
		return true;
	}
	void set_update_rate(int per_second) override {
		std::lock_guard<std::mutex> lock(m);
		update_rate = per_second < 1 ? 1 : (per_second > 200 ? 200 : per_second);
	}
	int get_update_rate() const override {
		std::lock_guard<std::mutex> lock(m);
		return update_rate;
	}
	sound_environment_source* create_source() override {
		// Called from the audio thread: never wait for the worker.
		std::unique_lock<std::mutex> lock(m, std::try_to_lock);
		if (!lock.owns_lock()) return nullptr;
		auto s = std::make_unique<sound_environment_source>();
		IPLSourceSettings settings {IPL_SIMULATIONFLAGS_DIRECT};
		if (iplSourceCreate(sim, &settings, &s->source) != IPL_STATUS_SUCCESS) return nullptr;
		sound_environment_source* result = s.get();
		sources.push_back(std::move(s));
		wake_requested = true;
		wake.notify_all();
		return result;
	}
	void release_source(sound_environment_source* s) override {
		if (s) s->active.store(false);
	}
	// Gives a new source the result simulated at the same spot recently, if there is one. Audio thread: never waits.
	bool try_prefill(sound_environment_source* s) {
		std::unique_lock<std::mutex> lock(m, std::try_to_lock);
		if (!lock.owns_lock()) return false;
		auto it = cache.find(cache_key(s->sx.load(), s->sy.load(), s->sz.load(), s->lx.load(), s->ly.load(), s->lz.load()));
		if (it == cache.end()) return false;
		const cached_result& r = it->second;
		s->occlusion.store(r.occlusion);
		for (int i = 0; i < 3; i++) s->transmission[i].store(r.transmission[i]);
		if (r.via_active) {
			s->via_amount.store(r.via_amount);
			for (int i = 0; i < 3; i++) s->via_eq[i].store(r.via_eq[i]);
			s->via_x.store(r.via_x);
			s->via_y.store(r.via_y);
			s->via_z.store(r.via_z);
			s->via_distance.store(r.via_distance);
		}
		s->via_active.store(r.via_active, std::memory_order_release);
		s->has_results.store(true, std::memory_order_release);
		return true;
	}
};
sound_environment* sound_environment::create() { return new sound_environment_impl(); }
bool sound_environment_try_prefill(sound_environment* env, sound_environment_source* s) { return env && s && static_cast<sound_environment_impl*>(env)->try_prefill(s); }
