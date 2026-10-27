#include "utils/ps5_hdr_helper.hpp"
#include <borealis/core/logger.hpp>
#include <cstring>

#if defined(PS5)
#include <unistd.h>

extern "C" {
int32_t ps5_get_videoout_handle(void);
void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t tiling,
                                    uint32_t width, uint32_t height, uint64_t reserved1,
                                    uint32_t reserved2, uint64_t reserved3);
int32_t sceVideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t set_index,
                                                const void *attribute, const void *option);
struct PS5OutputStatus {
    uint32_t resolution;
    uint32_t dynamic_range; // 0 unknown, 1 SDR, 2 HDR
    uint64_t refresh_rate;
    uint64_t flags;
    uint64_t reserved[3];
};
int32_t sceVideoOutGetOutputStatus(int32_t handle, struct PS5OutputStatus *status);
}

#define PS5_VIDEO_FORMAT_SDR UINT64_C(0x8000000000000000)
#define PS5_VIDEO_FORMAT_HDR UINT64_C(0x8100070400000000)

static bool s_hdrActive = false;

void PS5HDR::init() {
    s_hdrActive = false;
}

bool PS5HDR::setHdrOutput(bool enable) {
    if (enable == s_hdrActive) {
        return true;
    }

    int32_t handle = ps5_get_videoout_handle();
    if (handle < 0) {
        handle = 0;
    }

    uint64_t vfmt = enable ? PS5_VIDEO_FORMAT_HDR : PS5_VIDEO_FORMAT_SDR;
    brls::Logger::info("PS5HDR: switching scanout to {} (vfmt={:#x}, handle={})",
                       enable ? "HDR10 Bgr10A2Bt2100Pq" : "SDR Bgra8",
                       vfmt, handle);

    uint8_t attr[80];
    memset(attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute2(attr, vfmt, 0, 3840, 2160, 0, 0, 0);

    int32_t rc = sceVideoOutSubmitChangeBufferAttribute2(handle, 0, attr, nullptr);
    if (rc != 0 && handle == 0) {
        rc = sceVideoOutSubmitChangeBufferAttribute2(1, 0, attr, nullptr);
        if (rc == 0) handle = 1;
    }

    if (rc != 0) {
        brls::Logger::error("PS5HDR: SubmitChangeBufferAttribute2 failed rc=0x{:08x}", (uint32_t)rc);
        return false;
    }

    s_hdrActive = enable;
    brls::Logger::info("PS5HDR: scanout changed successfully, now {}", enable ? "HDR10" : "SDR");

    PS5OutputStatus status{};
    int32_t st_rc = sceVideoOutGetOutputStatus(handle, &status);
    brls::Logger::info("PS5HDR: output status rc={} dynamic_range={} ({}) flags={:#x}",
                       st_rc, status.dynamic_range,
                       status.dynamic_range == 2 ? "HDR" : (status.dynamic_range == 1 ? "SDR" : "unknown"),
                       status.flags);
    return true;
}

bool PS5HDR::isHdrOutputActive() {
    return s_hdrActive;
}

void PS5HDR::cleanup() {
    if (s_hdrActive) {
        setHdrOutput(false);
    }
}
#else
void PS5HDR::init() {}
bool PS5HDR::setHdrOutput(bool) { return false; }
bool PS5HDR::isHdrOutputActive() { return false; }
void PS5HDR::cleanup() {}
#endif
