#ifndef WINEHUA_SHARED_AUDIO_IPC_PROTOCOL_H
#define WINEHUA_SHARED_AUDIO_IPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINEHUA_AUDIO_PROTOCOL_VERSION 1u
#define WINEHUA_AUDIO_BOOTSTRAP_MAGIC 0x57484142u /* WHAB */
#define WINEHUA_AUDIO_CONTROL_MAGIC 0x57484143u   /* WHAC */
#define WINEHUA_AUDIO_RING_MAGIC 0x57484152u      /* WHAR */
#define WINEHUA_AUDIO_PROCESS_NAME_MAX 64u
#define WINEHUA_AUDIO_PACKET_MAX 512u

enum WinehuaAudioBootstrapCmd
{
    WINEHUA_AUDIO_BOOTSTRAP_OPEN_CONTROL = 1,
};

enum WinehuaAudioControlCmd
{
    WINEHUA_AUDIO_CMD_HELLO = 1,
    WINEHUA_AUDIO_CMD_OPEN_STREAM = 2,
    WINEHUA_AUDIO_CMD_START = 3,
    WINEHUA_AUDIO_CMD_STOP = 4,
    WINEHUA_AUDIO_CMD_RESET = 5,
    WINEHUA_AUDIO_CMD_CLOSE = 6,
    WINEHUA_AUDIO_CMD_GET_STATUS = 7,
};

enum WinehuaAudioSampleFormat
{
    WINEHUA_AUDIO_SAMPLE_S16LE = 1,
    WINEHUA_AUDIO_SAMPLE_FLOAT32 = 2,
};

enum WinehuaAudioStreamState
{
    WINEHUA_AUDIO_STREAM_STOPPED = 1,
    WINEHUA_AUDIO_STREAM_STARTED = 2,
    WINEHUA_AUDIO_STREAM_CLOSED = 3,
};

typedef struct WinehuaAudioBootstrapReq
{
    uint32_t magic;
    uint16_t version;
    uint16_t cmd;
    uint32_t client_pid;
    uint32_t client_tid;
    uint32_t flags;
    char process_name[WINEHUA_AUDIO_PROCESS_NAME_MAX];
} WinehuaAudioBootstrapReq;

typedef struct WinehuaAudioIpcHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t cmd;
    uint32_t seq;
    uint32_t payload_size;
} WinehuaAudioIpcHeader;

typedef struct WinehuaAudioHelloReq
{
    uint32_t client_pid;
    uint32_t client_tid;
    uint32_t flags;
    char process_name[WINEHUA_AUDIO_PROCESS_NAME_MAX];
} WinehuaAudioHelloReq;

typedef struct WinehuaAudioHelloResp
{
    int32_t result;
    uint32_t protocol_version;
    uint32_t mix_sample_rate;
    uint32_t mix_channels;
    uint32_t mix_sample_format;
} WinehuaAudioHelloResp;

typedef struct WinehuaAudioOpenStreamReq
{
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_format;
    uint32_t buffer_frames;
    uint32_t period_frames;
    uint32_t flags;
} WinehuaAudioOpenStreamReq;

typedef struct WinehuaAudioOpenStreamResp
{
    int32_t result;
    uint32_t stream_id;
    uint32_t mix_sample_rate;
    uint32_t mix_channels;
    uint32_t mix_sample_format;
    uint32_t mix_frame_size;
    uint32_t ring_capacity_frames;
    uint32_t ring_mapping_size;
    uint32_t preferred_period_frames;
} WinehuaAudioOpenStreamResp;

typedef struct WinehuaAudioStreamCmdReq
{
    uint32_t stream_id;
} WinehuaAudioStreamCmdReq;

typedef struct WinehuaAudioStreamCmdResp
{
    int32_t result;
    uint32_t stream_id;
} WinehuaAudioStreamCmdResp;

typedef struct WinehuaAudioGetStatusReq
{
    uint32_t stream_id;
} WinehuaAudioGetStatusReq;

typedef struct WinehuaAudioGetStatusResp
{
    int32_t result;
    uint32_t stream_id;
    uint32_t state;
    uint32_t queued_frames;
    uint32_t free_frames;
    uint32_t underrun_count;
    uint32_t overflow_count;
} WinehuaAudioGetStatusResp;

typedef struct WinehuaAudioRingBuffer
{
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_format;
    uint32_t frame_size;
    uint32_t capacity_frames;
    uint32_t reserved0;
    uint64_t read_index;
    uint64_t write_index;
    uint32_t state;
    uint32_t seq;
    uint32_t underrun_count;
    uint32_t overflow_count;
    uint32_t reserved1;
    uint32_t reserved2;
    uint8_t data[];
} WinehuaAudioRingBuffer;

static inline uint32_t winehua_audio_ring_header_size(void)
{
    return (uint32_t)offsetof(WinehuaAudioRingBuffer, data);
}

static inline uint64_t winehua_audio_ring_load_read_index(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->read_index, __ATOMIC_ACQUIRE);
}

static inline uint64_t winehua_audio_ring_load_write_index(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->write_index, __ATOMIC_ACQUIRE);
}

static inline void winehua_audio_ring_store_read_index(WinehuaAudioRingBuffer *ring, uint64_t value)
{
    __atomic_store_n(&ring->read_index, value, __ATOMIC_RELEASE);
}

static inline void winehua_audio_ring_store_write_index(WinehuaAudioRingBuffer *ring, uint64_t value)
{
    __atomic_store_n(&ring->write_index, value, __ATOMIC_RELEASE);
}

static inline uint32_t winehua_audio_ring_load_state(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->state, __ATOMIC_ACQUIRE);
}

static inline void winehua_audio_ring_store_state(WinehuaAudioRingBuffer *ring, uint32_t state)
{
    __atomic_store_n(&ring->state, state, __ATOMIC_RELEASE);
}

static inline uint32_t winehua_audio_ring_increment_seq(WinehuaAudioRingBuffer *ring)
{
    return __atomic_add_fetch(&ring->seq, 1u, __ATOMIC_ACQ_REL);
}

static inline uint32_t winehua_audio_ring_load_seq(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->seq, __ATOMIC_ACQUIRE);
}

static inline uint32_t winehua_audio_ring_load_underrun_count(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->underrun_count, __ATOMIC_RELAXED);
}

static inline uint32_t winehua_audio_ring_load_overflow_count(const WinehuaAudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->overflow_count, __ATOMIC_RELAXED);
}

static inline void winehua_audio_ring_increment_underrun(WinehuaAudioRingBuffer *ring)
{
    (void)__atomic_add_fetch(&ring->underrun_count, 1u, __ATOMIC_RELAXED);
}

static inline void winehua_audio_ring_increment_overflow(WinehuaAudioRingBuffer *ring)
{
    (void)__atomic_add_fetch(&ring->overflow_count, 1u, __ATOMIC_RELAXED);
}

static inline uint32_t winehua_audio_ring_used_frames(const WinehuaAudioRingBuffer *ring)
{
    uint64_t read_index = winehua_audio_ring_load_read_index(ring);
    uint64_t write_index = winehua_audio_ring_load_write_index(ring);
    uint64_t used = write_index - read_index;

    if (used > ring->capacity_frames) used = ring->capacity_frames;
    return (uint32_t)used;
}

static inline uint32_t winehua_audio_ring_free_frames(const WinehuaAudioRingBuffer *ring)
{
    return ring->capacity_frames - winehua_audio_ring_used_frames(ring);
}

static inline void winehua_audio_ring_reset(WinehuaAudioRingBuffer *ring, uint32_t state)
{
    winehua_audio_ring_store_read_index(ring, 0);
    winehua_audio_ring_store_write_index(ring, 0);
    __atomic_store_n(&ring->underrun_count, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->overflow_count, 0u, __ATOMIC_RELEASE);
    winehua_audio_ring_store_state(ring, state);
    winehua_audio_ring_increment_seq(ring);
}

static inline size_t winehua_audio_ring_write_frames(WinehuaAudioRingBuffer *ring,
                                                     const void *src,
                                                     size_t frames)
{
    const uint8_t *src_bytes = (const uint8_t *)src;
    uint64_t read_index = winehua_audio_ring_load_read_index(ring);
    uint64_t write_index = winehua_audio_ring_load_write_index(ring);
    uint64_t used = write_index - read_index;
    uint64_t free_frames = used >= ring->capacity_frames ? 0 : ring->capacity_frames - used;
    size_t to_write = frames > free_frames ? (size_t)free_frames : frames;
    size_t frame_offset;
    size_t first_frames;
    size_t first_bytes;
    size_t total_bytes;

    if (!to_write) return 0;

    frame_offset = (size_t)(write_index & (ring->capacity_frames - 1u));
    first_frames = ring->capacity_frames - frame_offset;
    if (first_frames > to_write) first_frames = to_write;

    first_bytes = first_frames * ring->frame_size;
    total_bytes = to_write * ring->frame_size;
    memcpy(ring->data + frame_offset * ring->frame_size, src_bytes, first_bytes);
    if (total_bytes > first_bytes)
        memcpy(ring->data, src_bytes + first_bytes, total_bytes - first_bytes);

    winehua_audio_ring_store_write_index(ring, write_index + to_write);
    return to_write;
}

static inline size_t winehua_audio_ring_read_frames(WinehuaAudioRingBuffer *ring,
                                                    void *dst,
                                                    size_t frames)
{
    uint8_t *dst_bytes = (uint8_t *)dst;
    uint64_t read_index = winehua_audio_ring_load_read_index(ring);
    uint64_t write_index = winehua_audio_ring_load_write_index(ring);
    uint64_t available = write_index - read_index;
    size_t to_read = frames > available ? (size_t)available : frames;
    size_t frame_offset;
    size_t first_frames;
    size_t first_bytes;
    size_t total_bytes;

    if (!to_read) return 0;

    frame_offset = (size_t)(read_index & (ring->capacity_frames - 1u));
    first_frames = ring->capacity_frames - frame_offset;
    if (first_frames > to_read) first_frames = to_read;

    first_bytes = first_frames * ring->frame_size;
    total_bytes = to_read * ring->frame_size;
    memcpy(dst_bytes, ring->data + frame_offset * ring->frame_size, first_bytes);
    if (total_bytes > first_bytes)
        memcpy(dst_bytes + first_bytes, ring->data, total_bytes - first_bytes);

    winehua_audio_ring_store_read_index(ring, read_index + to_read);
    return to_read;
}

#ifdef __cplusplus
}
#endif

#endif
