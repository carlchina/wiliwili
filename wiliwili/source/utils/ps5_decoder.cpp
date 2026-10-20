//
// Created by fang on 2026/10/20.
//
// The console's hardware video decoder. See ps5_decoder.hpp.
//
// The decoder lives in the graphics memory: the access units and the pictures
// are copied into memory the chip can read, and the pictures are copied out
// again. The layout of the chip's structures and the way it wants its memory
// come from the VLC for PS5 port (ZiZc3/VLC, GPL-2.0-or-later).
//

#ifdef PS5

#include "utils/ps5_decoder.hpp"

#include <cstring>
#include <mutex>

#include <borealis/core/logger.hpp>

namespace ps5decoder
{

namespace
{

// The chip's structures. Their sizes are fixed by the console.
enum
{
    VDEC_CODEC_AVC  = 1,
    VDEC_CODEC_HEVC = 974921,
};

struct VdecConfig
{
    uint64_t this_size;
    uint32_t resource_type;  // 1: decoding on a compute queue
    uint32_t codec_type;
    uint32_t profile;
    uint32_t max_level;
    int32_t max_frame_width;
    int32_t max_frame_height;
    int32_t max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void* compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t cpu_thread_priority;
    bool optimize_progressive_video;
    bool check_memory_type;
    uint8_t reserved0;
    uint8_t reserved1;
    void* extra_config_info;
};

struct VdecMemory
{
    uint64_t this_size;
    uint64_t cpu_memory_size;
    void* cpu_memory;
    uint64_t gpu_memory_size;
    void* gpu_memory;
    uint64_t cpu_gpu_memory_size;
    void* cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment;
    uint32_t reserved0;
};

struct VdecInput
{
    uint64_t this_size;
    void* au_data;
    uint64_t au_size;
    uint64_t pts_data;
    uint64_t dts_data;
    uint64_t attached_data;
};

struct VdecOutput
{
    uint64_t this_size;
    bool is_valid;
    bool is_error_frame;
    uint8_t picture_count;
    uint32_t codec_type;
    uint32_t frame_width;
    uint32_t frame_pitch;  // in samples
    uint32_t frame_height;
    void* frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format;
    uint32_t frame_pitch_in_bytes;
};

struct VdecFrameBuffer
{
    uint64_t this_size;
    void* frame_buffer;
    uint64_t frame_buffer_size;
    bool is_accepted;
};

struct VdecComputeMemory
{
    uint64_t this_size;
    uint64_t cpu_gpu_memory_size;
    void* cpu_gpu_memory;
};

struct VdecComputeConfig
{
    uint64_t this_size;
    uint16_t compute_pipe_id;
    uint16_t compute_queue_id;
    bool check_memory_type;
    uint8_t reserved0;
    uint16_t reserved1;
};

static_assert(sizeof(VdecConfig) == 0x48, "decoder config");
static_assert(sizeof(VdecMemory) == 0x48, "decoder memory");
static_assert(sizeof(VdecInput) == 0x30, "input");
static_assert(sizeof(VdecOutput) == 0x38, "output");
static_assert(sizeof(VdecFrameBuffer) == 0x20, "frame buffer");
static_assert(sizeof(VdecComputeMemory) == 0x18, "compute memory");
static_assert(sizeof(VdecComputeConfig) == 0x10, "compute config");

extern "C" {
int sceVideodec2QueryComputeMemoryInfo(VdecComputeMemory* info);
int sceVideodec2AllocateComputeQueue(const VdecComputeConfig* config, const VdecComputeMemory* memory,
                                     void** queue);
int sceVideodec2ReleaseComputeQueue(void* queue);
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig* config, VdecMemory* memory);
int sceVideodec2CreateDecoder(const VdecConfig* config, const VdecMemory* memory, void** decoder);
int sceVideodec2DeleteDecoder(void* decoder);
int sceVideodec2Decode(void* decoder, const VdecInput* input, VdecFrameBuffer* frame, VdecOutput* output);
int sceVideodec2Flush(void* decoder, VdecFrameBuffer* frame, VdecOutput* output);
int sceVideodec2Reset(void* decoder);
int sceVideodec2GetPictureInfo(const VdecOutput* output, void* first, void* second);

int sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment,
                                  int type, int64_t* start);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelMapFlexibleMemory(void** addr, size_t len, int prot, int flags);
int sceKernelReleaseFlexibleMemory(void* addr, size_t len);
int sceKernelMunmap(void* addr, size_t len);
}

constexpr int PROT_CPU_RW     = 0x03;
constexpr int PROT_GPU_RW     = 0x30;
constexpr int DIRECT_TYPE     = 12;
constexpr size_t PAGE_16K     = 0x4000;
constexpr int AU_SLOTS        = 8;
constexpr size_t AU_SLOT_SIZE = (size_t)4 << 20;

/**
 * A block of memory the chip can read. The chip's picture store and its
 * access units live in the graphics memory; its working memory can be plain
 * system memory.
 */
struct Mem
{
    void* addr   = nullptr;
    size_t len   = 0;
    int64_t start = -1;  // direct memory; -1: flexible; -2: the shared cpu block
};

size_t round16k(size_t value)
{
    return (value + PAGE_16K - 1) & ~(PAGE_16K - 1);
}

bool memDirect(Mem* mem, size_t len, size_t alignment, int prot)
{
    mem->len   = round16k(len);
    mem->start = -1;
    mem->addr  = nullptr;
    if (alignment < PAGE_16K)
        alignment = PAGE_16K;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), mem->len, alignment, DIRECT_TYPE,
                                      &mem->start) != 0)
        return false;
    if (sceKernelMapDirectMemory(&mem->addr, mem->len, prot, 0, mem->start, alignment) != 0) {
        sceKernelReleaseDirectMemory(mem->start, mem->len);
        mem->start = -1;
        mem->addr  = nullptr;
        return false;
    }
    return true;
}

bool memFlexible(Mem* mem, size_t len)
{
    mem->len   = round16k(len);
    mem->start = -1;
    mem->addr  = nullptr;
    return sceKernelMapFlexibleMemory(&mem->addr, mem->len, PROT_CPU_RW, 0) == 0;
}

/**
 * The decoder's working memory. Flexible memory that is given back does not
 * come back to the pool on this console, so one block is kept and reused.
 */
std::mutex g_cpuBlockLock;
void* g_cpuBlock     = nullptr;
size_t g_cpuBlockLen = 0;
bool g_cpuBlockBusy  = false;

bool memCpu(Mem* mem, size_t len)
{
    len = round16k(len);
    std::lock_guard<std::mutex> lock(g_cpuBlockLock);
    if (!g_cpuBlockBusy) {
        if (len > g_cpuBlockLen) {
            // Room for 4K HEVC's 76 MiB, so the block rarely has to grow.
            size_t want = len > ((size_t)96 << 20) ? len : ((size_t)96 << 20);
            void* addr   = nullptr;
            if (sceKernelMapFlexibleMemory(&addr, want, PROT_CPU_RW, 0) == 0) {
                g_cpuBlock     = addr;
                g_cpuBlockLen  = want;
            }
        }
        if (g_cpuBlock && len <= g_cpuBlockLen) {
            g_cpuBlockBusy = true;
            mem->addr      = g_cpuBlock;
            mem->len       = len;
            mem->start     = -2;
            return true;
        }
    }
    return memFlexible(mem, len);
}

void memFree(Mem* mem)
{
    if (!mem->addr)
        return;
    if (mem->start == -2) {
        std::lock_guard<std::mutex> lock(g_cpuBlockLock);
        g_cpuBlockBusy = false;
    } else if (mem->start >= 0) {
        sceKernelMunmap(mem->addr, mem->len);
        sceKernelReleaseDirectMemory(mem->start, mem->len);
    } else {
        sceKernelMunmap(mem->addr, mem->len);
    }
    mem->addr  = nullptr;
    mem->len   = 0;
    mem->start = -1;
}

}  // namespace

/**
 * The compute queue the decoders run on. One for the life of the
 * application: it has to be made before the application asks for full
 * access, the decoders made later are refused.
 */
namespace
{
std::mutex g_queueLock;
void* g_queue     = nullptr;
Mem g_queueMem    = {};

void* computeQueue()
{
    std::lock_guard<std::mutex> lock(g_queueLock);
    if (!g_queue) {
        brls::Logger::info("ps5 decoder: asking the console for the compute queue's memory");
        VdecComputeMemory info = {.this_size = sizeof(info)};
        int rc = sceVideodec2QueryComputeMemoryInfo(&info);
        brls::Logger::info("ps5 decoder: compute queue memory query: rc=0x{:08x}, {} KiB", rc,
                           info.cpu_gpu_memory_size >> 10);
        if (rc == 0 && memDirect(&g_queueMem, info.cpu_gpu_memory_size, PAGE_16K,
                                 PROT_CPU_RW | PROT_GPU_RW)) {
            info.cpu_gpu_memory      = g_queueMem.addr;
            info.cpu_gpu_memory_size = g_queueMem.len;
            VdecComputeConfig config = {.this_size = sizeof(config)};
            brls::Logger::info("ps5 decoder: making the compute queue");
            rc = sceVideodec2AllocateComputeQueue(&config, &info, &g_queue);
            brls::Logger::info("ps5 decoder: compute queue: rc=0x{:08x}", rc);
            if (rc != 0) {
                g_queue = nullptr;
                memFree(&g_queueMem);
            }
        } else if (rc == 0) {
            brls::Logger::error("ps5 decoder: no memory for the compute queue");
        }
        brls::Logger::info("ps5 decoder: compute queue {} (rc=0x{:08x}, {} KiB)",
                           g_queue ? "ready" : "FAILED", rc, info.cpu_gpu_memory_size >> 10);
    }
    return g_queue;
}

}  // namespace

struct Decoder::Impl
{
    void* decoder      = nullptr;
    Codec codec        = CODEC_NONE;
    bool tenBit        = false;
    unsigned frameCount = 0;
    size_t frameSize   = 0;
    bool frameHeld[32] = {};
    Mem frames         = {};
    Mem cpu            = {};
    Mem gpu            = {};
    Mem cpuGpu         = {};
    Mem au[AU_SLOTS]   = {};
};

Decoder::~Decoder()
{
    close();
}

bool Decoder::available()
{
    return computeQueue() != nullptr;
}

bool Decoder::open(Codec codec, int profile, int level, int width, int height)
{
    close();
    if (codec != CODEC_AVC && codec != CODEC_HEVC)
        return false;

    void* queue = computeQueue();
    if (!queue)
        return false;

    auto* impl = new Impl();
    impl->codec = codec;
    for (auto& slot : impl->au)
        slot.start = -1;

    VdecConfig config = {
        .this_size              = sizeof(config),
        .resource_type          = 1,
        .codec_type             = codec == CODEC_AVC ? VDEC_CODEC_AVC : VDEC_CODEC_HEVC,
        .profile                = (uint32_t)profile,
        .max_level              = (uint32_t)level,
        // The video's own size, rounded to 16; 4K when it is not known yet.
        .max_frame_width        = width ? (int32_t)((width + 15) & ~15) : 3840,
        .max_frame_height       = height ? (int32_t)((height + 15) & ~15) : 2176,
        // The decoder sizes its picture store for the stream itself.
        .max_dpb_frame_count    = -1,
        // Several pictures at once: one at a time left it idle between calls.
        .decode_pipeline_depth  = 4,
        .compute_queue          = queue,
        .cpu_affinity_mask      = 0x3f,
        .cpu_thread_priority    = 700,
        .optimize_progressive_video = true,
    };
    impl->frameCount = 10;
    impl->tenBit     = codec != CODEC_AVC && profile == 2;

    auto bringUp = [&](VdecConfig* cfg) -> bool {
        VdecMemory mem = {.this_size = sizeof(mem)};
        if (sceVideodec2QueryDecoderMemoryInfo(cfg, &mem) != 0) {
            brls::Logger::error("ps5 decoder: memory query failed");
            return false;
        }
        size_t alignment = mem.frame_buffer_alignment > PAGE_16K ? mem.frame_buffer_alignment : PAGE_16K;
        impl->frameSize  = (mem.max_frame_buffer_size + alignment - 1) & ~(alignment - 1);
        bool ok = (!mem.cpu_memory_size || memCpu(&impl->cpu, mem.cpu_memory_size)) &&
                  (!mem.gpu_memory_size ||
                   memDirect(&impl->gpu, mem.gpu_memory_size, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) &&
                  (!mem.cpu_gpu_memory_size ||
                   memDirect(&impl->cpuGpu, mem.cpu_gpu_memory_size, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW)) &&
                  memDirect(&impl->frames, impl->frameSize * impl->frameCount, alignment,
                            PROT_CPU_RW | PROT_GPU_RW);
        for (int i = 0; ok && i < AU_SLOTS; i++)
            ok = memDirect(&impl->au[i], AU_SLOT_SIZE, PAGE_16K, PROT_CPU_RW | PROT_GPU_RW);
        if (!ok) {
            brls::Logger::error("ps5 decoder: no memory for the decoder");
            for (auto& slot : impl->au)
                memFree(&slot);
            memFree(&impl->frames);
            memFree(&impl->cpuGpu);
            memFree(&impl->gpu);
            memFree(&impl->cpu);
            return false;
        }
        // The memory is new to the chip: a picture it did not decode fully
        // would show what was there before.
        if (impl->cpu.addr)
            memset(impl->cpu.addr, 0, impl->cpu.len);
        if (impl->gpu.addr)
            memset(impl->gpu.addr, 0, impl->gpu.len);
        if (impl->cpuGpu.addr)
            memset(impl->cpuGpu.addr, 0, impl->cpuGpu.len);
        if (impl->tenBit) {
            uint64_t* words = (uint64_t*)impl->frames.addr;
            for (size_t i = 0, n = impl->frames.len / 8; i < n; i++)
                words[i] = 0x0200020002000200ull;
        } else {
            memset(impl->frames.addr, 0x80, impl->frames.len);
        }
        mem.cpu_memory        = impl->cpu.addr;
        mem.gpu_memory        = impl->gpu.addr;
        mem.cpu_gpu_memory    = impl->cpuGpu.addr;
        void* decoder         = nullptr;
        if (sceVideodec2CreateDecoder(cfg, &mem, &decoder) != 0) {
            brls::Logger::error("ps5 decoder: create failed");
            for (auto& slot : impl->au)
                memFree(&slot);
            memFree(&impl->frames);
            memFree(&impl->cpuGpu);
            memFree(&impl->gpu);
            memFree(&impl->cpu);
            return false;
        }
        impl->decoder = decoder;
        return true;
    };

    bool ok = bringUp(&config);
    if (!ok) {
        // The pipeline depth costs memory: one picture at a time still plays
        // most videos.
        config.decode_pipeline_depth = 1;
        ok = bringUp(&config);
    }
    if (!ok) {
        // Small pictures were refused by the memory query while 1080p and 4K
        // never were: ask for room for 1080p, which still decodes them.
        config.max_frame_width  = config.max_frame_width > 1920 ? config.max_frame_width : 1920;
        config.max_frame_height = config.max_frame_height > 1088 ? config.max_frame_height : 1088;
        config.decode_pipeline_depth = 4;
        ok = bringUp(&config);
        if (!ok) {
            config.decode_pipeline_depth = 1;
            ok = bringUp(&config);
        }
    }
    if (!ok) {
        delete impl;
        return false;
    }

    brls::Logger::info("ps5 decoder: ready ({} {}x{}, {} pictures of {} KiB)",
                       codec == CODEC_AVC ? "H.264" : "HEVC", config.max_frame_width,
                       config.max_frame_height, impl->frameCount, impl->frameSize >> 10);
    this->impl = impl;
    return true;
}

bool Decoder::decode(const uint8_t* annexb, size_t size, uint64_t pts, Picture& picture, uint8_t* buffer,
                     size_t bufferSize)
{
    if (!impl || !impl->decoder || !annexb || size == 0)
        return false;
    if (size > AU_SLOT_SIZE)
        return false;

    // The chip reads the access unit from the graphics memory.
    Mem* slot = nullptr;
    for (auto& candidate : impl->au) {
        if (!candidate.addr) {
            slot = &candidate;
            break;
        }
    }
    if (!slot)
        return false;
    memcpy(slot->addr, annexb, size);

    VdecInput in = {
        .this_size  = sizeof(in),
        .au_data   = slot->addr,
        .au_size   = size,
        .pts_data  = pts,
        .dts_data  = UINT64_MAX,
    };
    VdecFrameBuffer fb = {.this_size = sizeof(fb)};
    VdecOutput out     = {.this_size = sizeof(out)};

    int rc = sceVideodec2Decode(impl->decoder, &in, &fb, &out);
    if (rc != 0) {
        brls::Logger::error("ps5 decoder: decode failed (0x{:08x})", rc);
        return false;
    }
    if (!out.is_valid)
        return false;

    // The picture is in the decoder's own memory: copy it out.
    size_t needed = impl->tenBit ? (size_t)out.frame_width * out.frame_height * 3
                                 : (size_t)out.frame_width * out.frame_height * 3 / 2;
    if (!out.frame_buffer || needed > bufferSize)
        return false;

    const uint8_t* src    = (const uint8_t*)out.frame_buffer;
    size_t pitch          = out.frame_pitch_in_bytes ? out.frame_pitch_in_bytes : out.frame_width;
    size_t lumaSize       = (size_t)pitch * out.frame_height;
    size_t chromaHeight   = impl->tenBit ? out.frame_height / 2 : out.frame_height / 2;
    memcpy(buffer, src, lumaSize);
    memcpy(buffer + lumaSize, src + lumaSize, (size_t)pitch * chromaHeight);

    picture.data   = buffer;
    picture.width  = (int)out.frame_width;
    picture.height = (int)out.frame_height;
    picture.pitch  = (int)pitch;
    picture.tenBit = impl->tenBit;
    picture.pts    = pts;
    return true;
}

bool Decoder::flush(Picture& picture, uint8_t* buffer, size_t bufferSize)
{
    if (!impl || !impl->decoder)
        return false;

    VdecFrameBuffer fb = {.this_size = sizeof(fb)};
    VdecOutput out     = {.this_size = sizeof(out)};
    int rc = sceVideodec2Flush(impl->decoder, &fb, &out);
    if (rc != 0 || !out.is_valid)
        return false;

    size_t needed = impl->tenBit ? (size_t)out.frame_width * out.frame_height * 3
                                 : (size_t)out.frame_width * out.frame_height * 3 / 2;
    if (!out.frame_buffer || needed > bufferSize)
        return false;

    const uint8_t* src  = (const uint8_t*)out.frame_buffer;
    size_t pitch        = out.frame_pitch_in_bytes ? out.frame_pitch_in_bytes : out.frame_width;
    size_t lumaSize     = (size_t)pitch * out.frame_height;
    size_t chromaHeight = out.frame_height / 2;
    memcpy(buffer, src, lumaSize);
    memcpy(buffer + lumaSize, src + lumaSize, (size_t)pitch * chromaHeight);

    picture.data   = buffer;
    picture.width  = (int)out.frame_width;
    picture.height = (int)out.frame_height;
    picture.pitch  = (int)pitch;
    picture.tenBit = impl->tenBit;
    picture.pts    = 0;
    return true;
}

void Decoder::close()
{
    if (!impl)
        return;
    if (impl->decoder)
        sceVideodec2DeleteDecoder(impl->decoder);
    for (auto& slot : impl->au)
        memFree(&slot);
    memFree(&impl->frames);
    memFree(&impl->cpuGpu);
    memFree(&impl->gpu);
    memFree(&impl->cpu);
    delete impl;
    impl = nullptr;
}

}  // namespace ps5decoder

/**
 * Makes the compute queue the decoders run on. It has to be made before the
 * application asks for full access: the console refuses one made later.
 */
extern "C" int wiliwili_ps5_decoder_prepare(void)
{
    return ps5decoder::computeQueue() != nullptr ? 1 : 0;
}

#endif  // PS5
