/* sound_nodes.h - audio nodes header
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

#include <exception>
#include <angelscript.h> // asAtomic
#include <miniaudio_phonon.h>
#include <phonon.h>
#include <unordered_map>
#include <vector>
#include "sound.h"

class audio_node_impl : public virtual audio_node {
protected:
	ma_node_base* node; // Must be set by subclasses
	audio_engine *engine;
	int refcount;
	std::vector<audio_node*> output_connections; // Owns a reference to whatever each output bus is currently attached to, so an attached node can't be freed out from under the graph.
public:
	audio_node_impl(ma_node_base *node, audio_engine *engine) : audio_node(), node(node), engine(engine), refcount(1) {
		if (!init_sound()) throw std::runtime_error("sound system was not initialized");
		if (!engine) this->engine = g_audio_engine;
		if (dynamic_cast<audio_node_impl*>(this->engine) != this) this->engine->duplicate();
	}
	~audio_node_impl() {
		for (audio_node *n : output_connections) if (n) n->release();
		if (engine && dynamic_cast<audio_node_impl*>(this->engine) != this) engine->release();
	}
	void duplicate() { asAtomicInc(refcount); }
	void release() {
		if (asAtomicDec(refcount) < 1)
			delete this;
	}
	audio_engine *get_engine() const { return engine; }
	ma_node_base *get_ma_node() { return node; }
	unsigned int get_input_bus_count() { return node ? ma_node_get_input_bus_count(node) : 0; }
	unsigned int get_output_bus_count() { return node ? ma_node_get_output_bus_count(node) : 0; }
	unsigned int get_input_channels(unsigned int bus) { return node ? ma_node_get_input_channels(node, bus) : 0; }
	unsigned int get_output_channels(unsigned int bus) { return node ? ma_node_get_output_channels(node, bus) : 0; }
	bool attach_output_bus(unsigned int output_bus, audio_node *destination, unsigned int destination_input_bus) {
		if (!node) return false;
		if ((g_soundsystem_last_error = ma_node_attach_output_bus(node, output_bus, destination->get_ma_node(), destination_input_bus)) != MA_SUCCESS) return false;
		if (output_bus >= output_connections.size()) output_connections.resize(output_bus + 1, nullptr);
		if (output_connections[output_bus]) output_connections[output_bus]->release();
		output_connections[output_bus] = destination;
		destination->duplicate();
		return true;
	}
	bool detach_output_bus(unsigned int bus) {
		if (!node) return false;
		if ((g_soundsystem_last_error = ma_node_detach_output_bus(node, bus)) != MA_SUCCESS) return false;
		if (bus < output_connections.size() && output_connections[bus]) {
			output_connections[bus]->release();
			output_connections[bus] = nullptr;
		}
		return true;
	}
	bool detach_all_output_buses() {
		if (!node) return false;
		if ((g_soundsystem_last_error = ma_node_detach_all_output_buses(node)) != MA_SUCCESS) return false;
		for (audio_node *&n : output_connections) { if (n) { n->release(); n = nullptr; } }
		return true;
	}
	bool set_output_bus_volume(unsigned int bus, float volume) { return node ? (g_soundsystem_last_error = ma_node_set_output_bus_volume(node, bus, volume)) == MA_SUCCESS : false; }
	float get_output_bus_volume(unsigned int bus) { return node ? ma_node_get_output_bus_volume(node, bus) : 0; }
	bool set_state(ma_node_state state) { return node ? (g_soundsystem_last_error = ma_node_set_state(node, state)) == MA_SUCCESS : false; }
	ma_node_state get_state() { return node ? ma_node_get_state(node) : ma_node_state_stopped; }
	bool set_state_time(ma_node_state state, unsigned long long time) { return node ? (g_soundsystem_last_error = ma_node_set_state_time(node, state, time)) == MA_SUCCESS : false; }
	unsigned long long get_state_time(ma_node_state state) { return node ? ma_node_get_state_time(node, state) : static_cast<unsigned long long>(ma_node_state_stopped); }
	ma_node_state get_state_by_time(unsigned long long global_time) { return node ? ma_node_get_state_by_time(node, global_time) : ma_node_state_stopped; }
	ma_node_state get_state_by_time_range(unsigned long long global_time_begin, unsigned long long global_time_end) { return node ? ma_node_get_state_by_time_range(node, global_time_begin, global_time_end) : ma_node_state_stopped; }
	unsigned long long get_time() const { return node ? ma_node_get_time(node) : 0; }
	bool set_time(unsigned long long local_time) { return node ? (g_soundsystem_last_error = ma_node_set_time(node, local_time)) == MA_SUCCESS : false; }
};
typedef struct {
	ma_node_base base;
	effect_node* node;
} ma_effect_node;
class effect_node_impl : public audio_node_impl, public virtual effect_node {
	ma_node_vtable vtable;
	public:
	std::unique_ptr<ma_effect_node> n;
	effect_node_impl(audio_engine* e, ma_uint8 input_channel_count, ma_uint8 output_channel_count, ma_uint8 input_bus_count, ma_uint8 output_bus_count, unsigned int flags);
	~effect_node_impl();
	void destroy_node();
	void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) override;
	unsigned int required_input_frame_count(unsigned int output_frame_count) const override;
};

class passthrough_node : public virtual audio_node {
	public:
	static passthrough_node* create(audio_engine* engine);
};

// When nodes are added or removed from a node_chain, they are automatically reattached as needed.
class audio_node_chain : public virtual passthrough_node {
public:
	virtual bool attach_output_bus(unsigned int bus_index, audio_node* node, unsigned int input_bus_index) override = 0;
	virtual bool detach_output_bus(unsigned int bus_index) override = 0;
	virtual bool detach_all_output_buses() override = 0;
	virtual bool add_node(audio_node* node, audio_node* after = nullptr, unsigned int input_bus_index = 0) = 0;
	virtual bool add_node_at(audio_node* node, int after = -1, unsigned int input_bus_index = 0) = 0;
	virtual bool remove_node(audio_node* node) = 0;
	virtual bool remove_node_at(unsigned int index) = 0;
	virtual bool clear(bool detach_nodes = true) = 0;
	virtual void set_endpoint(audio_node* endpoint, unsigned int input_bus_index = 0) = 0;
	virtual audio_node* get_endpoint() const = 0;
	virtual audio_node* first() const = 0;
	virtual audio_node* last() const = 0;
	virtual audio_node* operator[](unsigned int index) const = 0;
	virtual int index_of(audio_node* node) const = 0;
	virtual unsigned int get_node_count() const = 0;
	static audio_node_chain* create(audio_node* source = nullptr, audio_node* endpoint = nullptr, audio_engine* engine = nullptr);
};

bool set_global_hrtf(bool enabled);
bool get_global_hrtf();

class phonon_binaural_node : public virtual audio_node {
	public:
	static phonon_binaural_node* create(audio_engine* engine, int channels, int sample_rate, int frame_size = 0);
	virtual void set_direction(float x, float y, float z, float distance) = 0;
	virtual void set_direction_vector(const reactphysics3d::Vector3& direction, float distance) = 0;
	virtual void set_spatial_blend_max_distance(float max_distance) = 0;
};
class splitter_node : public virtual audio_node {
	public:
	static splitter_node* create(audio_engine* engine, int channels);
};
class low_pass_filter_node : public virtual audio_node {
public:
	virtual void set_cutoff_frequency(double freq) = 0;
	virtual double get_cutoff_frequency() const = 0;
	virtual void set_order(unsigned int order) = 0;
	virtual unsigned int get_order() const = 0;
	static low_pass_filter_node* create(double cutoff_frequency, unsigned int order, audio_engine* engine);
};
class high_pass_filter_node : public virtual audio_node {
public:
	virtual void set_cutoff_frequency(double freq) = 0;
	virtual double get_cutoff_frequency() const = 0;
	virtual void set_order(unsigned int order) = 0;
	virtual unsigned int get_order() const = 0;
	static high_pass_filter_node* create(double cutoff_frequency, unsigned int order, audio_engine* engine);
};
class band_pass_filter_node : public virtual audio_node {
public:
	virtual void set_cutoff_frequency(double freq) = 0;
	virtual double get_cutoff_frequency() const = 0;
	virtual void set_order(unsigned int order) = 0;
	virtual unsigned int get_order() const = 0;
	static band_pass_filter_node* create(double cutoff_frequency, unsigned int order, audio_engine* engine);
};
class notch_filter_node : public virtual audio_node {
public:
	virtual void set_q(double q) = 0;
	virtual double get_q() const = 0;
	virtual void set_frequency(double freq) = 0;
	virtual double get_frequency() const = 0;
	static notch_filter_node* create(double q, double frequency, audio_engine* engine);
};
class peak_filter_node : public virtual audio_node {
public:
	virtual void set_gain(double gain) = 0;
	virtual double get_gain() const = 0;
	virtual void set_q(double q) = 0;
	virtual double get_q() const = 0;
	virtual void set_frequency(double freq) = 0;
	virtual double get_frequency() const = 0;
	static peak_filter_node* create(double gain_db, double q, double frequency, audio_engine* engine);
};
class low_shelf_filter_node : public virtual audio_node {
public:
	virtual void set_gain(double gain) = 0;
	virtual double get_gain() const = 0;
	virtual void set_q(double q) = 0;
	virtual double get_q() const = 0;
	virtual void set_frequency(double freq) = 0;
	virtual double get_frequency() const = 0;
	static low_shelf_filter_node* create(double gain_db, double q, double frequency, audio_engine* engine);
};
class high_shelf_filter_node : public virtual audio_node {
public:
	virtual void set_gain(double gain) = 0;
	virtual double get_gain() const = 0;
	virtual void set_q(double q) = 0;
	virtual double get_q() const = 0;
	virtual void set_frequency(double freq) = 0;
	virtual double get_frequency() const = 0;
	static high_shelf_filter_node* create(double gain_db, double q, double frequency, audio_engine* engine);
};
class delay_node : public virtual audio_node {
public:
	virtual void set_wet(float wet) = 0;
	virtual float get_wet() const = 0;
	virtual void set_dry(float dry) = 0;
	virtual float get_dry() const = 0;
	virtual void set_decay(float decay) = 0;
	virtual float get_decay() const = 0;
	static delay_node* create(unsigned int delay_in_frames, float decay, audio_engine* engine);
};
class freeverb_node : public virtual audio_node {
	public:
	virtual void set_room_size(float size) = 0;
	virtual float get_room_size() const = 0;
	virtual void set_damping(float damping) = 0;
	virtual float get_damping() const = 0;
	virtual void set_width(float width) = 0;
	virtual float get_width() const = 0;
	virtual void set_wet(float wet) = 0;
	virtual float get_wet() const = 0;
	virtual void set_dry(float dry) = 0;
	virtual float get_dry() const = 0;
	virtual void set_input_width(float width) = 0;
	virtual float get_input_width() const = 0;
	virtual void set_frozen(bool frozen) = 0;
	virtual bool get_frozen() const = 0;
	static freeverb_node* create(audio_engine* engine);
};
class reverb3d : public virtual passthrough_node {
public:
	virtual void set_reverb(audio_node* verb) = 0;
	virtual audio_node* get_reverb() const = 0;
	virtual void set_mixer(mixer* mix) = 0;
	virtual mixer* get_mixer() const = 0;
	virtual void set_min_volume(float min_volume) = 0;
	virtual float get_min_volume() const = 0;
	virtual void set_max_volume(float max_volume) = 0;
	virtual float get_max_volume() const = 0;
	virtual void set_max_volume_distance(float distance) = 0;
	virtual float get_max_volume_distance() const = 0;
	virtual void set_max_audible_distance(float distance) = 0;
	virtual float get_max_audible_distance() const = 0;
	virtual void set_volume_curve(float volume_curve) = 0;
	virtual float get_volume_curve() const = 0;
	virtual float get_volume_at(float distance) const = 0;
	virtual splitter_node* create_attachment(audio_node* dry_input = nullptr, audio_node* dry_output = nullptr) = 0;
	static reverb3d* create(audio_node* reverb, mixer* destination = nullptr, audio_engine* e = g_audio_engine);
};
class plugin_node : public virtual audio_node {
public:
	virtual audio_plugin_node_interface* get_plugin_interface() const = 0;
	virtual void process(const float** frames_in, unsigned int* frame_count_in, float** frames_out, unsigned int* frame_count_out) = 0;
	static plugin_node* create(audio_plugin_node_interface* impl, unsigned char input_bus_count, unsigned char output_bus_count, unsigned int flags, audio_engine* engine);
};

enum audio_spatializer_distance_model {
	linear,
	inverse,
	exponential
};
struct audio_spatialization_parameters {
	float listener_x;
	float listener_y;
	float listener_z;
	float listener_direction_x;
	float listener_direction_y;
	float listener_direction_z;
	float listener_distance;
	float sound_x;
	float sound_y;
	float sound_z;
	float min_distance;
	float max_distance;
	float min_volume;
	float max_volume;
	float rolloff;
	float directional_attenuation_factor;
	audio_spatializer_distance_model distance_model;
};

class spatializer_component_node;

class audio_spatializer : public virtual audio_node_chain {
public:
	virtual void set_panner(spatializer_component_node* new_panner) = 0;
	virtual spatializer_component_node* get_panner() const = 0;
	virtual void set_attenuator(spatializer_component_node* new_attenuator) = 0;
	virtual spatializer_component_node* get_attenuator() const = 0;
	virtual void set_panner_by_id(int panner_id) = 0;
	virtual void set_attenuator_by_id(int attenuator_id) = 0;
	virtual int get_current_panner_id() const = 0;
	virtual int get_current_attenuator_id() const = 0;
	virtual int get_preferred_panner_id() const = 0;
	virtual int get_preferred_attenuator_id() const = 0;
	virtual void set_rolloff(float rolloff) = 0;
	virtual float get_rolloff() const = 0;
	virtual void set_directional_attenuation_factor(float factor) = 0;
	virtual float get_directional_attenuation_factor() const = 0;
	virtual void set_reverb3d(reverb3d* new_reverb, audio_spatializer_reverb3d_placement placement = postpan) = 0;
	virtual reverb3d* get_reverb3d() const = 0;
	virtual splitter_node* get_reverb3d_attachment() const = 0;
	virtual audio_spatializer_reverb3d_placement get_reverb3d_placement() const = 0;
	virtual mixer* get_mixer() const = 0;
	virtual bool get_parameters(audio_spatialization_parameters& params) = 0;
	virtual void on_panner_enabled_changed(int panner_id, bool enabled) = 0;
	virtual void on_attenuator_enabled_changed(int attenuator_id, bool enabled) = 0;
	static audio_spatializer* create(mixer* mixer, audio_engine* engine = nullptr);
};

class spatializer_component_node : public virtual effect_node {
public:
	virtual audio_spatializer* get_spatializer() const = 0;
};

class basic_panner : public virtual spatializer_component_node {
public:
	static spatializer_component_node* create(audio_spatializer* spatializer, audio_engine* engine);
};

class phonon_hrtf_panner : public virtual spatializer_component_node {
public:
	static spatializer_component_node* create(audio_spatializer* spatializer, audio_engine* engine);
};

class basic_attenuator : public virtual spatializer_component_node {
public:
	static spatializer_component_node* create(audio_spatializer* spatializer, audio_engine* engine);
};

class phonon_attenuator : public virtual spatializer_component_node {
public:
	static spatializer_component_node* create(audio_spatializer* spatializer, audio_engine* engine);
};

typedef spatializer_component_node* (*spatializer_component_node_factory)(audio_spatializer*, audio_engine*);

struct spatializer_component {
	spatializer_component_node_factory factory;
	bool enabled;
};

int register_audio_panner(spatializer_component_node_factory factory, bool default_enabled = true);
int register_audio_attenuator(spatializer_component_node_factory factory, bool default_enabled = true);
spatializer_component_node* create_audio_panner(int id, audio_spatializer* spatializer, audio_engine* engine);
spatializer_component_node* create_audio_attenuator(int id, audio_spatializer* spatializer, audio_engine* engine);
void sound_set_default_3d_panner(int panner_id);
int sound_get_default_3d_panner();
void sound_set_default_3d_attenuator(int attenuator_id);
int sound_get_default_3d_attenuator();
void set_audio_panner_enabled(int id, bool enabled);
void set_audio_attenuator_enabled(int id, bool enabled);
bool get_audio_panner_enabled(int id);
bool get_audio_attenuator_enabled(int id);
bool sound_set_spatialization(int panner, int attenuator, bool disable_previous = false, bool set_default = true);


extern int g_audio_basic_panner;
extern int g_audio_phonon_hrtf_panner;
extern int g_audio_basic_attenuator;
extern int g_audio_phonon_attenuator;

// sound_environment: walls and other geometry that sounds are heard through (occlusion) and muffled by (transmission).
// Build it from boxes of materials, attach it to a mixer (every 3D sound in that mixer uses it) or to a single sound, and the phonon attenuator
// quietens and muffles each sound by how much geometry lies between it and the listener. A background thread runs Steam Audio's direct
// simulation a few times a second; the audio thread only reads its latest results. Unlike the legacy plugin's sound_environment, there are
// no simulated reflections (they're expensive per source; use reverb3d or a mixer reverb for echo), transmission is actually applied, and
// occlusion is volumetric by default, so sounds fade in and out of occlusion instead of cutting.
struct sound_environment_source; // Per sound; created and owned by the environment, see sound_nodes.cpp.
class sound_environment {
public:
	virtual void add_ref() = 0;
	virtual void release() = 0;
	// Materials: absorption (unused without reflections, but kept for compatibility), scattering, and how much of each band gets through (0 to 1).
	virtual bool add_material(const std::string& name, float absorption_low, float absorption_mid, float absorption_high, float scattering, float transmission_low, float transmission_mid, float transmission_high, bool replace_if_existing = false) = 0;
	virtual bool material_exists(const std::string& name) const = 0;
	// Boxes: returns an id, or -1 if the material doesn't exist. A disabled box (an open door) is left out of the scene until it's enabled again.
	virtual int add_box(const std::string& material, float minx, float maxx, float miny, float maxy, float minz, float maxz, bool enabled = true) = 0;
	virtual bool remove_box(int id) = 0;
	virtual bool set_box_enabled(int id, bool enabled) = 0;
	virtual bool get_box_enabled(int id) const = 0;
	virtual void clear_boxes() = 0;
	virtual unsigned int get_box_count() const = 0;
	// Occlusion: sources are spheres of occlusion_radius, sampled with occlusion_samples rays (1 = a single ray, which cuts in and out).
	virtual void set_occlusion_radius(float radius) = 0;
	virtual float get_occlusion_radius() const = 0;
	virtual void set_occlusion_samples(int samples) = 0;
	virtual int get_occlusion_samples() const = 0;
	// How many surfaces between source and listener count towards transmission.
	virtual void set_transmission_rays(int rays) = 0;
	virtual int get_transmission_rays() const = 0;
	// Portals: openings (doorways) sound can come round through when the straight line is blocked. A blocked sound that can be seen
	// from a portal the listener can see is heard through that portal: duller the sharper the bend, and from the portal's direction
	// (see sound.get_portal_route). radius is about half the opening's width. Disable a portal when its door closes.
	virtual int add_portal(float x, float y, float z, float radius) = 0;
	virtual bool remove_portal(int id) = 0;
	virtual bool set_portal_enabled(int id, bool enabled) = 0;
	virtual bool get_portal_enabled(int id) const = 0;
	virtual unsigned int get_portal_count() const = 0;
	// The listener's position in this environment's world. Until it's set, the audio engine's listener is used; clear_listener goes back to that.
	// Set it when sounds are positioned relative to the listener (sound_pool), along with each sound's set_occlusion_position.
	virtual void set_listener(float x, float y, float z) = 0;
	virtual void clear_listener() = 0;
	virtual bool get_listener(float& x, float& y, float& z) const = 0;
	// Simulations per second.
	virtual void set_update_rate(int per_second) = 0;
	virtual int get_update_rate() const = 0;
	// Used by mixers and the phonon attenuator.
	virtual sound_environment_source* create_source() = 0; // Returns null if the environment is busy; try again next block.
	virtual void release_source(sound_environment_source* source) = 0;
	static sound_environment* create();
};
// Per-sound simulation results, for the phonon attenuator. All thread safe.
void sound_environment_source_set_positions(sound_environment_source* source, float sound_x, float sound_y, float sound_z, float listener_x, float listener_y, float listener_z);
void sound_environment_source_get_results(sound_environment_source* source, float& occlusion, float transmission[3]);
// The route through a portal, if the sound is currently heard that way: the portal's position and the total distance listener to portal to sound.
bool sound_environment_source_get_route(sound_environment_source* source, float& x, float& y, float& z, float& distance);
