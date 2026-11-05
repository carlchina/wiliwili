//
// Created by fang on 2023/11/14.
//

#include <borealis/core/thread.hpp>
#include <borealis/core/logger.hpp>

#include "presenter/video_detail.hpp"
#include "presenter/live_data.hpp"
#include "utils/config_helper.hpp"
#include "bilibili.h"
#include "api/bilibili/util/http.hpp"

bool LiveDataRequest::selectStream(const bilibili::LiveRoomPlayInfo& playInfo, int protocolPreference) {
    const auto& playurl = playInfo.playurl_info.playurl;
    if (playurl.stream.empty()) return false;

    // 协议选择偏好：
    // protocolPreference == 1: 优先 HLS (http_hls)，次选 FLV (http_stream)
    // 否则: 优先 FLV (http_stream)，次选 HLS (http_hls)
    std::vector<std::string> protocolOrder;
    if (protocolPreference == 1) {
        protocolOrder = {"http_hls", "http_stream"};
    } else {
        protocolOrder = {"http_stream", "http_hls"};
    }

    // 编码偏好：
    // 默认优先 avc (兼容性最好，不黑屏)，其次 hevc，最后 av1
    // 若用户显式配置了 VIDEO_CODEC (7: AVC, 12: HEVC, 13: AV1)，遵从用户偏好
    std::vector<std::string> codecOrder = {"avc", "hevc", "av1"};
    if (bilibili::BilibiliClient::VIDEO_CODEC == 12) {
        codecOrder = {"hevc", "avc", "av1"};
    } else if (bilibili::BilibiliClient::VIDEO_CODEC == 13) {
        codecOrder = {"av1", "avc", "hevc"};
    }

    auto findCodec = [&](const bilibili::LiveStreamFormat& fmt) -> std::optional<bilibili::LiveStreamFormatCodec> {
        for (const auto& targetCodec : codecOrder) {
            for (const auto& c : fmt.codec) {
                if (c.codec_name == targetCodec && !c.url_info.empty()) {
                    return c;
                }
            }
        }
        for (const auto& c : fmt.codec) {
            if (!c.url_info.empty()) return c;
        }
        return std::nullopt;
    };

    // 1. 按协议顺序和格式顺序遍历
    for (const auto& protoName : protocolOrder) {
        for (const auto& s : playurl.stream) {
            if (s.protocol_name != protoName) continue;

            // 格式偏好：HLS 优先 fmp4，次选 ts；FLV 优先 flv
            std::vector<std::string> formatOrder;
            if (protoName == "http_hls") {
                formatOrder = {"fmp4", "ts"};
            } else {
                formatOrder = {"flv"};
            }

            for (const auto& fmtName : formatOrder) {
                for (const auto& f : s.format) {
                    if (f.format_name != fmtName) continue;
                    auto c = findCodec(f);
                    if (c.has_value()) {
                        liveUrl = *c;
                        brls::Logger::info("Selected live stream: protocol={} format={} codec={} qn={}",
                                           protoName, fmtName, c->codec_name, c->current_qn);
                        return true;
                    }
                }
            }

            // 若指定 format 未命中，遍历该 protocol 下的任意 format
            for (const auto& f : s.format) {
                auto c = findCodec(f);
                if (c.has_value()) {
                    liveUrl = *c;
                    brls::Logger::info("Selected live stream (fallback format): protocol={} format={} codec={} qn={}",
                                       protoName, f.format_name, c->codec_name, c->current_qn);
                    return true;
                }
            }
        }
    }

    // 2. 全局兜底：任意可用 stream 的任意 format 和 codec
    for (const auto& s : playurl.stream) {
        for (const auto& f : s.format) {
            for (const auto& c : f.codec) {
                if (!c.url_info.empty()) {
                    liveUrl = c;
                    brls::Logger::warning("Selected live stream (loose fallback): protocol={} format={} codec={} qn={}",
                                          s.protocol_name, f.format_name, c.codec_name, c.current_qn);
                    return true;
                }
            }
        }
    }

    return false;
}

void LiveDataRequest::requestData(int roomid, int protocolPreference) {
    ASYNC_RETAIN
    BILI::get_live_room_play_info(
        roomid, defaultQuality,
        [ASYNC_TOKEN, protocolPreference](const auto& result) {
            liveRoomPlayInfo = result;
            qualityDescriptionMap.clear();
            for (auto& i : liveRoomPlayInfo.playurl_info.playurl.g_qn_desc) {
                qualityDescriptionMap[i.qn] = i.desc;
            }

            if (!selectStream(liveRoomPlayInfo, protocolPreference)) {
                brls::Logger::error("No usable live stream found for room");
            }

            brls::sync([ASYNC_TOKEN]() {
                ASYNC_RELEASE
                onLiveData(liveRoomPlayInfo);
            });
        },
        [ASYNC_TOKEN](BILI_ERR) {
            brls::sync([ASYNC_TOKEN, error]() {
                ASYNC_RELEASE
                this->onError(error);
            });
        });

    reportHistory(roomid);
}

void LiveDataRequest::reportHistory(int roomid) {
    // 复用视频播放页面的标记
    if (!VideoDetail::REPORT_HISTORY) return;

    BILI::report_live_history(
        roomid, ProgramConfig::instance().getCSRF(),
        [roomid]() { brls::Logger::debug("report live history {}", roomid); },
        [](BILI_ERR) { brls::Logger::error("report live history: {}", error); });
}

void LiveDataRequest::requestPayLiveInfo(int roomid) {
    ASYNC_RETAIN
    BILI::get_live_pay_info(
        roomid,
        [ASYNC_TOKEN, roomid](const auto& payInfo) {
            brls::Logger::debug("get live pay info {}", payInfo.permission);
            if (payInfo.permission != 0) return;
            BILI::get_live_pay_link(
                roomid,
                [ASYNC_TOKEN, payInfo](const auto& payLink) {
                    brls::Logger::debug("get live pay link {}", payLink.goods_link);
                    brls::sync([ASYNC_TOKEN, payInfo, payLink]() {
                        ASYNC_RELEASE
                        this->onNeedPay(payInfo.message, payLink.goods_link, payLink.start_time, payLink.end_time);
                    });
                },
                [ASYNC_TOKEN, payInfo](BILI_ERR) {
                    brls::Logger::error("get live pay link:", error);
                    brls::sync([ASYNC_TOKEN, payInfo]() {
                        ASYNC_RELEASE
                        this->onNeedPay(payInfo.message, "", "", "");
                    });
                });
        },
        [ASYNC_TOKEN](BILI_ERR) {
            ASYNC_RELEASE
            brls::Logger::error("get live pay info: {}", error);
        });
}

void LiveDataRequest::requestLiveDanmakuToken(int roomid) {
    ASYNC_RETAIN
    BILI::get_live_danmaku_info(
        roomid,
        [ASYNC_TOKEN, roomid](const auto& info) {
            brls::sync([ASYNC_TOKEN, roomid, info]() {
                ASYNC_RELEASE
                this->onDanmakuInfo(roomid, info);
            });
        },
        [ASYNC_TOKEN](BILI_ERR) {
            ASYNC_RELEASE
            brls::Logger::error("get live danmaku info: {}", error);
        });
}

std::string LiveDataRequest::getQualityDescription(int qn) {
    if (qualityDescriptionMap.count(qn) == 0) return "Unknown Quality " + std::to_string(qn);
    return qualityDescriptionMap[qn];
}

void LiveDataRequest::requestLiveAnchorInfo(int roomid) {
    if (liveRoomPlayInfo.uid > 0) {
        // 从liveRoomPlayInfo中获取的数据
        std::string uid = std::to_string(liveRoomPlayInfo.uid);
        brls::Logger::debug("get live anchor info, uid: {}", uid);
        
        // 调用已有的用户卡片API
        std::vector<std::string> uids = {uid};
        ASYNC_RETAIN
        BILI::get_user_cards(
            uids,
            [ASYNC_TOKEN](const bilibili::UserCardListResult& result) {
                if (!result.empty()) {
                    std::string face = result[0].face;
                    std::string uname = result[0].name;
                    
                    brls::sync([ASYNC_TOKEN, face, uname]() {
                        ASYNC_RELEASE
                        this->onAnchorInfo(face, uname);
                    });
                } else {
                    brls::Logger::error("get live anchor info: empty result");
                    ASYNC_RELEASE
                }
            },
            [ASYNC_TOKEN](BILI_ERR) {
                brls::Logger::error("get live anchor info: {}", error);
                brls::sync([ASYNC_TOKEN]() {
                    ASYNC_RELEASE
                    this->onAnchorInfo("", "主播");
                });
            }
        );
    } else {
        brls::Logger::error("get live anchor info: no uid available");
        this->onAnchorInfo("", "主播");
    }
}

// 新增：获取主播称号信息
void LiveDataRequest::requestLiveAnchorTitle(int roomid) {
    brls::Logger::debug("requesting anchor title for room: {}", roomid);
    
    ASYNC_RETAIN
    cpr::Parameters params = {{"roomid", std::to_string(roomid)}};
    cpr::Url url = cpr::Url{"https://api.live.bilibili.com/live_user/v1/UserInfo/get_anchor_in_room"};
    
    auto session = bilibili::HTTP::createSession();
    session->SetUrl(url);
    session->SetParameters(params);
    
    session->GetCallback<>(
        [ASYNC_TOKEN](const cpr::Response& r) {
            if (r.status_code == 200) {
                try {
                    auto json = nlohmann::json::parse(r.text);
                    std::string title = "";
                    
                    if (json.contains("data") && json["data"].contains("info") && 
                        json["data"]["info"].contains("official_verify") && 
                        json["data"]["info"]["official_verify"].contains("desc")) {
                        title = json["data"]["info"]["official_verify"]["desc"].get<std::string>();
                    }
                    
                    brls::sync([ASYNC_TOKEN, title]() {
                        ASYNC_RELEASE
                        this->onAnchorTitleInfo(title);
                    });
                } catch (const std::exception& e) {
                    brls::Logger::error("Failed to parse anchor title data: {}", e.what());
                    ASYNC_RELEASE
                }
            } else {
                brls::Logger::error("Failed to get anchor title: HTTP {}", r.status_code);
                ASYNC_RELEASE
            }
        });
}