#pragma once

#include <cstdint>

/// PS5 的物理 HDR10 输出。
///
/// 帧缓冲永远是 8bit 的 B8G8R8A8，显示端靠 sceVideoOutSubmitChangeBufferAttribute2
/// 把同一块内存重新解释成 10bit PQ。所以“着色器按 PQ 打包”和“显示端真的切成 10bit”
/// 必须成对出现：切换失败时一定要让调用方知道，否则整屏颜色都会花掉。
class PS5HDR {
public:
    static void init();
    static bool setHdrOutput(bool enable);
    static bool isHdrOutputActive();

    /// 一次播放结束：当前是 HDR 就切回 SDR，并让下一条视频重新尝试切换。
    static void endPlayback();

    /// 程序退出：若仍是 HDR 则恢复 SDR，避免把桌面留在 HDR 模式。
    static void cleanup();
};
