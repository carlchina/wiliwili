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

// 0: 还没切换过, 1: 已切到 HDR10, 2: 已切到 SDR, 3: HDR 切换失败（本次播放不再重试）
//
// 切换失败时不能每帧重试：mpv 每帧都会问状态，失败一次就会刷两条日志，
// 几秒钟就能把 8MB 的日志写满并翻转。
static int s_state = 0;

void PS5HDR::init() {
    s_state = 0;
}

bool PS5HDR::setHdrOutput(bool enable) {
    if (enable && s_state == 1) {
        return true;
    }
    if (enable && s_state == 3) {
        return false;
    }
    if (!enable && (s_state == 0 || s_state == 2)) {
        s_state = 2;
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
        brls::Logger::error("PS5HDR: SubmitChangeBufferAttribute2 failed rc=0x{:08x} (handle={})",
                            (uint32_t)rc, handle);
        if (enable) {
            // 显示端没有切到 10bit PQ，但帧缓冲还是 8bit：此时若继续按 PQ 打包，
            // 整屏（连界面一起）都会变成黄紫色。降级到 SDR 色调映射，并把原因留在日志里。
            brls::Logger::error("PS5HDR: staying on SDR output, HDR is tone mapped in the shader");
            s_state = 3;
        } else {
            s_state = 2;
        }
        return false;
    }

    s_state = enable ? 1 : 2;
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
    return s_state == 1;
}

void PS5HDR::endPlayback() {
    if (s_state == 1) {
        setHdrOutput(false);
    } else {
        // 没切过 HDR（或切换失败过）：扫描输出不用动，只清掉状态让下一条视频重新尝试
        s_state = 0;
    }
}

void PS5HDR::cleanup() {
    if (s_state == 1) {
        setHdrOutput(false);
    }
}
#else
void PS5HDR::init() {}
bool PS5HDR::setHdrOutput(bool) { return false; }
bool PS5HDR::isHdrOutputActive() { return false; }
void PS5HDR::endPlayback() {}
void PS5HDR::cleanup() {}
#endif
