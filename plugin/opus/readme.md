# NVGT Opus Plugin — Agent Reference Guide

This guide is a complete reference for the `opus` plugin. It is written for agents and developers alike. Every type, method, property, and constant is documented, and full working examples are provided for every major use case including real-time voice chat.

---

## Table of Contents

1. [Overview](#overview)
2. [Distribution and Building](#distribution-and-building)
3. [Loading the Plugin](#loading-the-plugin)
4. [Constants and Enums](#constants-and-enums)
5. [opus_encoder](#opus_encoder)
6. [opus_decoder](#opus_decoder)
7. [Packet Utility Functions](#packet-utility-functions)
8. [opus_file_writer](#opus_file_writer)
9. [opus_file_reader](#opus_file_reader)
10. [Voice Chat — Complete Example](#voice-chat--complete-example)
11. [File Encode / Decode — Complete Example](#file-encode--decode--complete-example)
12. [Error Handling Patterns](#error-handling-patterns)
13. [Integration with NVGT Sound System](#integration-with-nvgt-sound-system)
14. [Performance Notes](#performance-notes)

---

## Distribution and Building

### Pre-built — nothing to do

The plugin is **already compiled and ships with NVGT**. After building NVGT normally (or using a release package), the file is at:

```
release/lib/opus.dll       (Windows)
release/lib/libopus.so     (Linux)
release/lib/libopus.dylib  (macOS)
```

Copy or symlink this file into the `lib/` folder of any NVGT project and it is ready to use. No separate build step is required.

### Rebuilding from source

If you need to recompile the plugin (e.g. after modifying `plugin/opus/opus.cpp`), run SCons from the repository root. Build only the shared library to avoid recompiling the full engine:

```
scons release/lib/opus.dll no_stubs=1
```

On Linux/macOS substitute the appropriate output filename. SCons picks up the plugin automatically via `plugin/opus/_SConscript` — no changes to any other build file are needed.

To rebuild the entire project including the plugin:

```
scons no_stubs=1
```

The plugin depends on `opus`, `opusfile`, `opusenc`, and `ogg` — all of which are managed by NVGT's vcpkg setup and are already present in `windev/lib` (Windows) or the platform-equivalent dependency directory. No extra dependency installation is required.

---

## Overview

The `opus` plugin exposes four script types and four global utility functions:

| Type | Purpose |
|---|---|
| `opus_encoder` | Encode raw PCM frames to Opus packets (real-time, low-latency) |
| `opus_decoder` | Decode Opus packets back to raw PCM |
| `opus_file_writer` | Write audio to an `.opus` (OggOpus container) file |
| `opus_file_reader` | Read and seek `.opus` files, decoding to PCM |

All types use NVGT `string` as the PCM/packet data container (strings are byte buffers). Two PCM formats are supported throughout:

- **int16 LE** — 16-bit signed little-endian, range `[-32768, 32767]`, 2 bytes per sample
- **float32 LE** — IEEE 754 single precision, range `[-1.0, 1.0]`, 4 bytes per sample

Interleaved layout is used for multi-channel audio: `[L0, R0, L1, R1, ...]`.

---

## Loading the Plugin

Place `opus.dll` (Windows), `libopus.so` (Linux), or `libopus.dylib` (macOS) in the `lib/` folder next to your game or the NVGT executable.

```angelscript
// At the top of your script, before any opus types are used:
#pragma plugin opus
```

Or load dynamically at runtime:

```angelscript
if (!plugin_load("lib/opus"))
    alert("error", "Failed to load opus plugin");
```

---

## Constants and Enums

The engine already registers these enums in `sound.cpp`. Use them by name in any script that has the sound system active (which is always true in NVGT).

### Application types (`opus_application_type`)

| Constant | Value | Use when |
|---|---|---|
| `OPUS_APPLICATION_VOIP` | 2048 | Voice chat, speech — optimises for voice, enables speech enhancement |
| `OPUS_APPLICATION_AUDIO` | 2049 | Music, sound effects — best for general audio quality |
| `OPUS_APPLICATION_RESTRICTED_LOWDELAY` | 2051 | Game audio, live mixing — lowest latency, disables look-ahead |

### Signal type hints (`opus_signal_type`)

| Constant | Value | Effect |
|---|---|---|
| `OPUS_AUTO` | -1000 | Let the encoder decide automatically |
| `OPUS_SIGNAL_VOICE` | 3001 | Tune noise suppression and bandwidth for speech |
| `OPUS_SIGNAL_MUSIC` | 3002 | Tune for music fidelity |

### Bandwidth constants (raw integers — not registered as an enum)

| Constant | Value | Frequency range |
|---|---|---|
| `OPUS_AUTO` | -1000 | No limit (use for bandwidth too) |
| `OPUS_BANDWIDTH_NARROWBAND` | 1101 | 0–4 kHz |
| `OPUS_BANDWIDTH_MEDIUMBAND` | 1102 | 0–6 kHz |
| `OPUS_BANDWIDTH_WIDEBAND` | 1103 | 0–8 kHz |
| `OPUS_BANDWIDTH_SUPERWIDEBAND` | 1104 | 0–12 kHz |
| `OPUS_BANDWIDTH_FULLBAND` | 1105 | 0–20 kHz |

Pass these as raw integers to `set_bandwidth()` and `set_application()`.

---

## opus_encoder

Encodes PCM audio frames into compressed Opus packets. Each call to `encode()` or `encode_float()` produces one self-contained packet suitable for transmission or storage.

### Construction

```angelscript
opus_encoder@ enc(int sample_rate = 48000, int channels = 1, int application = OPUS_APPLICATION_VOIP)
```

| Parameter | Description |
|---|---|
| `sample_rate` | Must be 8000, 12000, 16000, 24000, or 48000. 48000 recommended. |
| `channels` | 1 (mono) or 2 (stereo) |
| `application` | One of the application type constants above |

```angelscript
opus_encoder@ enc = opus_encoder(48000, 1, OPUS_APPLICATION_VOIP); // voice
opus_encoder@ enc = opus_encoder(48000, 2, OPUS_APPLICATION_AUDIO); // stereo music
```

### Encoding methods

```angelscript
string encode(const string &in pcm_int16)
```
Encodes a buffer of int16 LE PCM. The number of samples per channel must be a valid Opus frame size:
- 120, 240, 480, 960 (2.5ms, 5ms, 10ms, 20ms) at 48000 Hz — 960 (20ms) is the standard choice
- 960 samples × 1 channel × 2 bytes = **1920 bytes** per call

Returns the encoded Opus packet as a string of bytes. Returns empty string on error — check `last_error`.

```angelscript
string encode_float(const string &in pcm_float32)
```
Same as `encode()` but accepts float32 LE PCM. Each sample is 4 bytes in `[-1.0, 1.0]`.
- 960 samples × 1 channel × 4 bytes = **3840 bytes** per call

### Properties and controls

All setters return `bool` (true on success). All getters are read-only properties.

| Method / Property | Type | Description |
|---|---|---|
| `set_bitrate(int bps)` / `bitrate` | int | Bits per second. Range: 500–512000. Typical: 8000 (narrow voice) – 128000 (stereo music). Default: auto (~32k mono voice). `OPUS_AUTO = -1000` restores auto. |
| `set_complexity(int c)` / `complexity` | int | CPU vs quality trade-off. 0 = fastest/lowest quality, 10 = slowest/best. Default: 9. For real-time use, 5–7 is common. |
| `set_vbr(bool on)` / `vbr` | bool | Variable bitrate. Default: true. Set false for CBR (constant bitrate streaming). |
| `set_vbr_constraint(bool on)` / `vbr_constraint` | bool | Constrained VBR — never exceeds the target bitrate. Useful for streaming. Default: false. |
| `set_dtx(bool on)` / `dtx` | bool | Discontinuous transmission. When true, silence produces ~2-byte packets. Saves bandwidth during pauses. Default: false. |
| `set_inband_fec(bool on)` / `inband_fec` | bool | In-band Forward Error Correction. Encodes redundant data for the previous packet inside the current packet. Enables `conceal()` on the decoder to recover from lost packets. Default: false. |
| `set_expected_loss(int pct)` / `expected_loss` | int | Expected packet loss percentage 0–100. Tunes FEC strength. Only relevant when `inband_fec` is true. Default: 0. |
| `set_signal(int sig)` / `signal` | int | Signal type hint. Use `OPUS_SIGNAL_VOICE` or `OPUS_SIGNAL_MUSIC`. Default: `OPUS_AUTO`. |
| `set_bandwidth(int bw)` / `bandwidth` | int | Maximum audio bandwidth. Use bandwidth constants above. Default: `OPUS_AUTO` (fullband). |
| `reset_state()` | bool | Resets encoder state without destroying it. Use between unrelated audio sessions. |
| `valid` | bool (read-only) | True if the encoder was created successfully. |
| `last_error` | string (read-only) | Human-readable description of the last error. |
| `sample_rate` | int (const) | Sample rate passed to constructor. |
| `channels` | int (const) | Channel count passed to constructor. |

### Recommended settings by use case

```angelscript
// Voice chat — low latency, packet loss tolerant
opus_encoder@ voice_enc = opus_encoder(48000, 1, OPUS_APPLICATION_VOIP);
voice_enc.set_bitrate(24000);
voice_enc.set_complexity(5);
voice_enc.set_dtx(true);         // silence compression
voice_enc.set_inband_fec(true);  // recover from lost packets
voice_enc.set_expected_loss(5);  // assume 5% packet loss
voice_enc.set_signal(OPUS_SIGNAL_VOICE);

// Music streaming — quality priority
opus_encoder@ music_enc = opus_encoder(48000, 2, OPUS_APPLICATION_AUDIO);
music_enc.set_bitrate(128000);
music_enc.set_complexity(10);
music_enc.set_vbr(true);
music_enc.set_signal(OPUS_SIGNAL_MUSIC);

// Game real-time audio — lowest latency
opus_encoder@ game_enc = opus_encoder(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
game_enc.set_bitrate(32000);
game_enc.set_complexity(5);
```

---

## opus_decoder

Decodes Opus packets back to PCM. One decoder instance per stream. The decoder maintains state between calls — always feed packets in order.

### Construction

```angelscript
opus_decoder@ dec(int sample_rate = 48000, int channels = 1)
```

The sample rate and channel count must match what the encoder used. For file decoding via `opusfile`, these are always 48000 and whatever the file reports.

```angelscript
opus_decoder@ dec = opus_decoder(48000, 1);
```

### Decoding methods

```angelscript
string decode(const string &in packet, int frame_size = 960)
```
Decodes an Opus packet to int16 LE PCM. `frame_size` is the expected number of samples per channel in the output. For standard 20ms frames at 48000 Hz this is 960. Returns empty string on error.

```angelscript
string decode_float(const string &in packet, int frame_size = 960)
```
Same but returns float32 LE PCM.

```angelscript
string conceal(int frame_size = 960)
```
Packet Loss Concealment (PLC). Call this when a packet was lost instead of calling `decode()`. Uses the encoder's FEC data (if `inband_fec` was enabled) or synthesises plausible audio to fill the gap. Returns float32 LE PCM. Keeps the decoder state consistent so subsequent packets decode correctly.

### Properties and controls

| Method / Property | Type | Description |
|---|---|---|
| `set_gain(int q8)` / `gain` | int | Output gain in Q8 dB (256 = +6 dB, -256 = -6 dB, 0 = unity). Applies to all decoded output. |
| `last_packet_duration` | int (read-only) | Number of samples per channel decoded in the last `decode()` call. |
| `bandwidth` | int (read-only) | Bandwidth of the last decoded packet (one of the bandwidth constants). |
| `reset_state()` | bool | Reset decoder state. Call when the stream is interrupted and you're resuming from a new point. |
| `valid` | bool (read-only) | True if created successfully. |
| `last_error` | string (read-only) | Description of last error. |
| `sample_rate` | int (const) | Sample rate passed to constructor. |
| `channels` | int (const) | Channel count passed to constructor. |

---

## Packet Utility Functions

These are global functions. They inspect Opus packet headers without needing an encoder or decoder instance.

```angelscript
int opus_packet_frames(const string &in packet)
```
Returns the number of Opus frames in the packet (usually 1 for standard 20ms packets). Returns negative on error.

```angelscript
int opus_packet_samples(const string &in packet, int sample_rate)
```
Returns total PCM samples per channel across all frames in the packet. For a 20ms packet at 48000 Hz: 960. Returns negative on error.

```angelscript
int opus_packet_bandwidth(const string &in packet)
```
Returns the bandwidth constant for this packet (1101–1105). Tells you the highest frequency content the encoder produced.

```angelscript
int opus_packet_channels(const string &in packet)
```
Returns 1 or 2. Note: this reads from the packet header — it may differ from the decoder's configured channel count if streams are mismatched.

**Usage:**
```angelscript
// Before feeding a packet to the decoder, validate it:
int samp = opus_packet_samples(packet, 48000);
if (samp <= 0) {
    // bad packet — call dec.conceal() instead
} else {
    string pcm = dec.decode(packet, samp);
}
```

---

## opus_file_writer

Writes audio to an OggOpus (`.opus`) file. Comments/tags must be added before the first `write()` call. Always call `close()` when done — the file is not finalised until then. The destructor will call `close()` automatically if you forget, but explicit `close()` lets you check for errors.

### Construction

```angelscript
opus_file_writer@ writer(const string &in path, int sample_rate = 48000, int channels = 1)
```

`sample_rate` can be any value (the file stores the input rate, though the Opus codec internally always uses 48000). `channels` is 1 or 2.

### Methods

```angelscript
bool write(const string &in pcm_int16)
```
Write a chunk of int16 LE PCM. No size restriction — write any number of samples at a time. The library handles framing internally.

```angelscript
bool write_float(const string &in pcm_float32)
```
Same but float32 LE PCM in `[-1.0, 1.0]`.

```angelscript
bool close()
```
Flush, write the Ogg end-of-stream page, and finalise the file. The object becomes invalid after this. Returns false on I/O error.

```angelscript
bool add_comment(const string &in tag, const string &in value)
```
Add a Vorbis comment tag to the file. Must be called before the first `write()`. Common tags: `TITLE`, `ARTIST`, `ALBUM`, `DATE`, `TRACKNUMBER`, `COMMENT`.

### Controls (can be changed between writes)

| Method | Description |
|---|---|
| `set_bitrate(int bps)` | Target bitrate. Default: 128000. |
| `set_complexity(int 0-10)` | Encoder complexity. Default: 10 (max quality). |
| `set_vbr(bool)` | Variable bitrate. Default: true. |
| `set_cvbr(bool)` | Constrained VBR. Default: false. |
| `set_dtx(bool)` | Discontinuous transmission. Default: false. |
| `set_application(int)` | Application type. Default: `OPUS_APPLICATION_AUDIO`. |
| `set_signal(int)` | Signal hint. Default: `OPUS_AUTO`. |

### Properties

| Property | Description |
|---|---|
| `valid` | True while open and not yet closed. |
| `last_error` | Human-readable OPE error string. |
| `sample_rate` | Value passed to constructor. |
| `channels` | Value passed to constructor. |

---

## opus_file_reader

Reads and decodes an OggOpus file. The output sample rate is always 48000 Hz regardless of the file's stored rate. Supports seeking in seekable files.

### Construction

```angelscript
opus_file_reader@ reader(const string &in path)
```

```angelscript
opus_file_reader@ r = opus_file_reader("recording.opus");
if (!r.valid) {
    alert("error", r.last_error);
    return;
}
```

### Methods

```angelscript
string read(int samples_per_channel)
```
Read up to `samples_per_channel` samples, returns int16 LE interleaved PCM. Returns fewer samples at end of file. Returns empty string on error.

```angelscript
string read_float(int samples_per_channel)
```
Same, returns float32 LE PCM.

```angelscript
string read_all()
```
Read the entire file as int16 LE PCM in one call. Uses `pcm_total` to pre-allocate. For very long files prefer chunked `read()`.

```angelscript
string read_all_float()
```
Same, returns float32.

```angelscript
bool seek(int64 sample_offset)
```
Seek to an absolute sample position (samples per channel from start). Returns false if the stream is not seekable or the position is out of range.

```angelscript
int64 tell()
```
Returns the current sample position.

### Properties

| Property | Description |
|---|---|
| `pcm_total` | Total samples per channel in the file. -1 if unknown (non-seekable streams). |
| `channels` | Channel count from the file header (1 or 2). |
| `sample_rate` | Always 48000 — opusfile always decodes at 48 kHz. |
| `valid` | True if the file opened successfully. |
| `last_error` | Human-readable opusfile error string. |

---

## Voice Chat — Complete Example

This example shows a complete architecture for real-time voice chat over a network using NVGT's `network` object alongside the opus plugin.

### Key parameters for voice chat

- **Frame size:** 960 samples (20ms at 48000 Hz) — balance between latency and compression efficiency
- **Application:** `OPUS_APPLICATION_VOIP`
- **Bitrate:** 16000–32000 bps for voice
- **DTX:** true — silence sends tiny 2-byte packets
- **FEC:** true — enables concealment of lost packets
- **Expected loss:** 3–10% depending on network conditions

### Sender side

```angelscript
#pragma plugin opus

// One encoder per outbound voice stream.
opus_encoder@ mic_enc;
network@ net;

bool voice_init(string host, uint16 port) {
    @mic_enc = opus_encoder(48000, 1, OPUS_APPLICATION_VOIP);
    if (!mic_enc.valid) return false;
    mic_enc.set_bitrate(24000);
    mic_enc.set_complexity(5);    // lighter CPU load for real-time
    mic_enc.set_dtx(true);        // no bandwidth wasted on silence
    mic_enc.set_inband_fec(true); // help the receiver recover lost packets
    mic_enc.set_expected_loss(5); // tune FEC for ~5% loss
    mic_enc.set_signal(OPUS_SIGNAL_VOICE);

    @net = network();
    return net.connect(1, host, port, 0);
}

// Call this from your audio capture callback, once per 20ms mic frame.
// pcm_frame: 1920 bytes of int16 LE mono PCM at 48000 Hz.
void voice_send(string pcm_frame) {
    string packet = mic_enc.encode(pcm_frame);
    if (packet.empty()) return; // DTX silence suppression or error

    // Build a simple wire packet: [1 byte type][2 bytes length][N bytes opus]
    network_packet@ pkt = network_packet();
    pkt.write_uint8(1);               // message type: voice
    pkt.write_uint16(packet.length());
    pkt.write_string_raw(packet);
    net.send(1, pkt, false);          // unreliable channel for voice
}
```

### Receiver side

```angelscript
#pragma plugin opus

// One decoder and one playout buffer per remote peer.
class voice_peer {
    opus_decoder@ dec;
    uint64 expected_sequence;

    voice_peer() {
        @dec = opus_decoder(48000, 1);
        expected_sequence = 0;
    }

    // Call when a voice packet arrives from this peer.
    // Returns int16 LE PCM ready to feed to stream_pcm or a mixer.
    string on_packet(string opus_packet) {
        if (!dec.valid) return "";
        string pcm = dec.decode(opus_packet);
        if (pcm.empty()) {
            // Decode error — conceal the frame
            return dec.conceal(960);
        }
        return pcm;
    }

    // Call when a packet was detected as lost (gap in sequence numbers).
    string on_lost() {
        if (!dec.valid) return "";
        return dec.conceal(960); // synthesise audio to fill the gap
    }
}

// In your network receive loop:
void voice_receive(network_packet@ pkt, voice_peer@ peer) {
    uint8 msg_type = pkt.read_uint8();
    if (msg_type != 1) return;
    uint16 len = pkt.read_uint16();
    string opus_data = pkt.read_string_raw(len);
    string pcm = peer.on_packet(opus_data);
    if (!pcm.empty())
        play_pcm_frame(pcm); // feed to your audio output
}
```

### Audio playout

NVGT's `sound` object can play raw PCM via `stream_pcm`. Feed decoded frames there:

```angelscript
sound voice_out;

void play_pcm_frame(string pcm_int16) {
    // stream_pcm expects: data, sample_rate, channels
    voice_out.stream_pcm(pcm_int16, 48000, 1);
}
```

### Sequence number handling for packet loss detection

```angelscript
// When sending, prepend a 4-byte sequence number to each packet.
uint32 send_seq = 0;

void voice_send_sequenced(string pcm_frame) {
    string packet = mic_enc.encode(pcm_frame);
    if (packet.empty()) return;

    network_packet@ pkt = network_packet();
    pkt.write_uint8(1);
    pkt.write_uint32(send_seq++);
    pkt.write_uint16(packet.length());
    pkt.write_string_raw(packet);
    net.send(1, pkt, false);
}

// When receiving, detect gaps:
uint32 recv_seq = 0;

void voice_receive_sequenced(network_packet@ pkt, voice_peer@ peer) {
    pkt.read_uint8(); // type
    uint32 seq = pkt.read_uint32();
    uint16 len = pkt.read_uint16();
    string opus_data = pkt.read_string_raw(len);

    // Conceal any missing packets before this one
    while (recv_seq < seq) {
        string concealed = peer.on_lost();
        if (!concealed.empty()) play_pcm_frame(concealed);
        recv_seq++;
    }
    recv_seq = seq + 1;

    string pcm = peer.on_packet(opus_data);
    if (!pcm.empty()) play_pcm_frame(pcm);
}
```

---

## File Encode / Decode — Complete Example

### Recording to a file

```angelscript
#pragma plugin opus

opus_file_writer@ recorder;

bool recording_start(string filename) {
    @recorder = opus_file_writer(filename, 48000, 1);
    if (!recorder.valid) {
        alert("error", "Cannot open file: " + recorder.last_error);
        return false;
    }
    recorder.add_comment("TITLE", "Recording");
    recorder.add_comment("DATE", date_time_now());
    recorder.set_bitrate(96000);
    recorder.set_complexity(10);
    recorder.set_vbr(true);
    recorder.set_application(OPUS_APPLICATION_AUDIO);
    return true;
}

// Feed raw mic PCM (any chunk size) during recording.
void recording_write(string pcm_int16) {
    if (recorder !is null && recorder.valid)
        recorder.write(pcm_int16);
}

void recording_stop() {
    if (recorder !is null && recorder.valid)
        recorder.close();
    @recorder = null;
}
```

### Playing back a file

```angelscript
#pragma plugin opus

void playback_file(string filename) {
    opus_file_reader@ r = opus_file_reader(filename);
    if (!r.valid) {
        alert("error", r.last_error);
        return;
    }

    println("Duration: " + r.pcm_total / 48000 + " seconds");
    println("Channels: " + r.channels);

    sound s;
    const int CHUNK = 48000 / 10; // 100ms chunks

    while (true) {
        string pcm = r.read(CHUNK);
        if (pcm.empty()) break;
        s.stream_pcm(pcm, 48000, r.channels);
    }
}
```

### Transcoding: re-encode at a different bitrate

```angelscript
#pragma plugin opus

void transcode(string input_path, string output_path, int target_bitrate) {
    opus_file_reader@ r = opus_file_reader(input_path);
    if (!r.valid) { alert("error", r.last_error); return; }

    opus_file_writer@ w = opus_file_writer(output_path, 48000, r.channels);
    if (!w.valid) { alert("error", w.last_error); return; }

    w.set_bitrate(target_bitrate);
    w.set_complexity(10);

    const int CHUNK = 4800; // 100ms at 48000 Hz
    while (true) {
        string pcm = r.read(CHUNK);
        if (pcm.empty()) break;
        w.write(pcm);
    }

    w.close();
    println("Transcoded to " + target_bitrate + " bps: " + output_path);
}
```

### Seeking and extracting a clip

```angelscript
#pragma plugin opus

// Extract samples [start_sec, end_sec) from a file.
string extract_clip(string path, double start_sec, double end_sec) {
    opus_file_reader@ r = opus_file_reader(path);
    if (!r.valid) return "";

    int64 start_sample = int64(start_sec * 48000.0);
    int64 end_sample   = int64(end_sec   * 48000.0);
    int64 num_samples  = end_sample - start_sample;

    if (!r.seek(start_sample)) return "";
    return r.read(int(num_samples));
}
```

---

## Error Handling Patterns

Every type exposes `valid` (bool) and `last_error` (string).

```angelscript
// Always check valid after construction:
opus_encoder@ enc = opus_encoder(48000, 1);
if (!enc.valid) {
    // Common causes: invalid sample_rate, invalid channel count
    alert("error", "Encoder init failed: " + enc.last_error);
    return;
}

// Check return values from setters:
if (!enc.set_bitrate(999999999)) // out of range
    println("set_bitrate failed: " + enc.last_error);

// Check encode/decode return values:
string packet = enc.encode(pcm);
if (packet.empty())
    println("encode failed: " + enc.last_error);

// File writer — check after close too:
opus_file_writer@ w = opus_file_writer("out.opus", 48000, 1);
w.write(pcm);
if (!w.close())
    println("finalize failed: " + w.last_error);

// File reader — check valid AND after reads:
opus_file_reader@ r = opus_file_reader("in.opus");
if (!r.valid) {
    println(r.last_error); // "not an Ogg Opus stream", "read error", etc.
}
```

### Opus error codes (returned by packet utilities)

Negative return values from `opus_packet_*` functions map to these codes:

| Value | Meaning |
|---|---|
| -1 | `OPUS_BAD_ARG` — invalid argument |
| -2 | `OPUS_BUFFER_TOO_SMALL` |
| -3 | `OPUS_INTERNAL_ERROR` |
| -4 | `OPUS_INVALID_PACKET` — empty or corrupt packet |
| -5 | `OPUS_UNIMPLEMENTED` |
| -6 | `OPUS_INVALID_STATE` — decoder/encoder not initialised |
| -7 | `OPUS_ALLOC_FAIL` |

---

## Integration with NVGT Sound System

The NVGT engine already has native OggOpus support via `audio_opus_encoder` (for recording to `.opus` files via the mixer). The plugin complements this with:

- Raw packet codec for **network streaming** (engine has no built-in voice chat)
- Direct file read with **seeking** (engine uses miniaudio backend, no programmatic seek)
- **Programmatic PCM extraction** from `.opus` files for effects processing

### Feeding decoded PCM to `sound.stream_pcm`

```angelscript
sound s;
opus_file_reader@ r = opus_file_reader("voice.opus");
const int CHUNK = 4800; // 100ms

timer t;
while (true) {
    if (t.elapsed >= 90) { // refill before buffer runs dry
        string pcm = r.read(CHUNK);
        if (pcm.empty()) break;
        s.stream_pcm(pcm, 48000, r.channels);
        t.restart();
    }
    wait(5);
}
```

### Using decoded PCM with `audio_decoder`

If you need effects processing (pitch, reverb, etc.) on an `.opus` file, decode it with `opus_file_reader` to get raw PCM and then feed it through NVGT's DSP chain.

---

## Performance Notes

- **Frame size 960 (20ms)** is the sweet spot for voice chat — smaller frames increase overhead, larger frames increase latency.
- **Complexity 5–7** is appropriate for real-time use on most hardware. Use 10 only for offline encoding.
- **DTX** reduces bandwidth by ~60–70% during silence — always enable it for voice chat.
- **One encoder/decoder instance per stream** — they maintain state between calls. Do not share instances across threads without locking.
- **`read_all()`** pre-allocates based on `pcm_total`. For files longer than a few minutes, prefer chunked reading to avoid large heap allocations.
- The opus codec always operates at 48000 Hz internally. Encoders created at 8000/16000/24000 Hz are valid but the data is resampled internally.
- **Packets are self-contained** — each `encode()` call produces one independently decodable packet. There is no dependency between packets (unlike MP3 frames), which makes Opus well suited for packet-switched networks.
