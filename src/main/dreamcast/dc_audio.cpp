// Dreamcast AICA audio backend
//
// Replaces the SDL audio subsystem with KallistiOS snd_stream for PCM
// audio output on the Dreamcast's Yamaha AICA sound processor.
//
// The AICA has its own 2 MB of dedicated sound RAM and an ARM7TDMI core
// for mixing. We use the snd_stream API which handles DMA transfers and
// double-buffering internally.

#ifdef DREAMCAST

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <array>
#include <atomic>

#include <kos.h>
#include <dc/sound/stream.h>

#include "dreamcast_platform.h"

namespace {

// ── Ring buffer for audio samples ───────────────────────────────────
// The game thread queues samples here; the AICA stream callback drains them.

constexpr size_t RING_BUFFER_FRAMES = 4096;   // Frames (stereo pairs)
constexpr size_t RING_BUFFER_CHANNELS = 2;
constexpr size_t RING_BUFFER_SIZE = RING_BUFFER_FRAMES * RING_BUFFER_CHANNELS;

static int16_t ring_buffer[RING_BUFFER_SIZE];
static std::atomic<size_t> ring_write_pos{0};
static std::atomic<size_t> ring_read_pos{0};
static snd_stream_hnd_t stream_handle = SND_STREAM_INVALID;

static uint32_t current_sample_rate = 48000;
// Volume scaling factor applied to all queued samples.
// TODO: Wire this to zelda64::get_main_volume() once the config system is
// fully integrated on Dreamcast.
static float volume_scale = 1.0f;

size_t ring_available() {
    size_t w = ring_write_pos.load(std::memory_order_acquire);
    size_t r = ring_read_pos.load(std::memory_order_acquire);
    if (w >= r) return w - r;
    return RING_BUFFER_SIZE - r + w;
}

size_t ring_free() {
    return RING_BUFFER_SIZE - 1 - ring_available();
}

// ── snd_stream callback ─────────────────────────────────────────────
// Called by the AICA DMA engine when it needs more samples.
// `smp_req` is the number of *bytes* requested. We fill `buf` with
// interleaved 16-bit stereo PCM.

void* stream_callback(snd_stream_hnd_t hnd, int smp_req, int* smp_recv) {
    (void)hnd;

    static int16_t callback_buffer[8192]; // Enough for typical requests
    size_t samples_requested = smp_req / sizeof(int16_t);
    if (samples_requested > sizeof(callback_buffer) / sizeof(int16_t)) {
        samples_requested = sizeof(callback_buffer) / sizeof(int16_t);
    }

    size_t avail = ring_available();
    size_t to_copy = (samples_requested < avail) ? samples_requested : avail;

    size_t r = ring_read_pos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < to_copy; i++) {
        callback_buffer[i] = ring_buffer[(r + i) % RING_BUFFER_SIZE];
    }

    // Fill remainder with silence if we underran
    if (to_copy < samples_requested) {
        memset(&callback_buffer[to_copy], 0, (samples_requested - to_copy) * sizeof(int16_t));
    }

    ring_read_pos.store((r + to_copy) % RING_BUFFER_SIZE, std::memory_order_release);

    *smp_recv = samples_requested * sizeof(int16_t);
    return callback_buffer;
}

} // anonymous namespace

namespace dreamcast {

void aica_init(uint32_t sample_rate) {
    current_sample_rate = sample_rate;

    snd_stream_init();
    stream_handle = snd_stream_alloc(stream_callback, 8192);
    if (stream_handle == SND_STREAM_INVALID) {
        fprintf(stderr, "[DC] Failed to allocate sound stream\n");
        return;
    }

    snd_stream_start(stream_handle, sample_rate, 1 /* stereo */);
    fprintf(stdout, "[DC] AICA audio initialized at %u Hz\n", sample_rate);
}

void aica_shutdown() {
    if (stream_handle != SND_STREAM_INVALID) {
        snd_stream_stop(stream_handle);
        snd_stream_destroy(stream_handle);
        stream_handle = SND_STREAM_INVALID;
    }
    snd_stream_shutdown();
}

void aica_queue_samples(const int16_t* samples, size_t sample_count) {
    // Convert from the game's format (16-bit interleaved stereo with swapped
    // channels due to N64 endianness) to normal interleaved stereo.
    // Apply volume scaling.

    size_t free = ring_free();
    size_t to_write = (sample_count < free) ? sample_count : free;

    size_t w = ring_write_pos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < to_write; i += 2) {
        // Swap channels (same as the SDL path in main.cpp)
        int32_t left  = static_cast<int32_t>(samples[i + 1]);
        int32_t right = static_cast<int32_t>(samples[i + 0]);

        // Apply volume
        left  = static_cast<int32_t>(left * volume_scale);
        right = static_cast<int32_t>(right * volume_scale);

        // Clamp
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;

        ring_buffer[(w + i + 0) % RING_BUFFER_SIZE] = static_cast<int16_t>(left);
        ring_buffer[(w + i + 1) % RING_BUFFER_SIZE] = static_cast<int16_t>(right);
    }

    ring_write_pos.store((w + to_write) % RING_BUFFER_SIZE, std::memory_order_release);
}

size_t aica_get_frames_remaining() {
    // Return buffered frame count (each frame = 2 samples for stereo)
    return ring_available() / RING_BUFFER_CHANNELS;
}

void aica_set_frequency(uint32_t freq) {
    current_sample_rate = freq;

    // Restart the stream at the new sample rate
    if (stream_handle != SND_STREAM_INVALID) {
        snd_stream_stop(stream_handle);
        snd_stream_start(stream_handle, freq, 1 /* stereo */);
    }
}

} // namespace dreamcast

#endif // DREAMCAST
