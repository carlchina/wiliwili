/**

██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██  █  ██ ██ ██      ██ ██  █  ██ ██ ██      ██
██ ███ ██ ██ ██      ██ ██ ███ ██ ██ ██      ██
 ███ ███  ██ ███████ ██  ███ ███  ██ ███████ ██

 Licensed under the GPL-3.0 license
*/

#include <borealis.hpp>

#include "utils/config_helper.hpp"
#include "utils/stage.hpp"
#include "utils/activity_helper.hpp"
#include "utils/ps5_decoder.hpp"
#include "view/mpv_core.hpp"

#ifdef IOS
#include <SDL2/SDL_main.h>
#endif

#ifdef PS5
// The console unloads a title through its system service.
extern "C" int sceSystemServiceLoadExec(const char* path, const char* argv[]);
// The handle the platform kept for the console's own log (runtime_shims.c).
extern "C" int wiliwili_console_log;
// Writes how far the start-up got, before the log can be written (runtime_shims.c).
extern "C" void wiliwili_mark(const char *stage);
#endif

int main(int argc, char* argv[]) {
#ifdef PS5
    wiliwili_mark("main");
#endif
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::TERMINAL = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "wiliwili.log";
            brls::Logger::setLogOutput(std::fopen(path, "w+"));
        }
    }

#ifdef PS5
    // The console title cannot take command line options; keep a debug log to
    // diagnose the remaining network layers through the crash reporter fetch.
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    // The log is a file on the console's storage and nothing removes it: bound
    // it so a long session cannot fill the drive with debug lines.
    brls::Logger::setLogFileLimit(16 * 1024 * 1024);
    // Warnings and errors go to the console's own log as well, so they are
    // visible while the title runs (port 3232 of the console) and are not lost
    // when the title stops without reaching the file.
    brls::Logger::setConsoleLogHandle(wiliwili_console_log);
    // The queue the console's hardware video decoder runs on has to be made
    // before the application asks for full access, and the crash reporter has
    // to be there to say what went wrong.
    ps5decoder::wiliwili_ps5_decoder_prepare();
#endif

    // Load cookies and settings
    ProgramConfig::instance().init();
#ifdef PS5
    wiliwili_mark("config");
#endif

#ifdef PS5
    // The console's hardware video decoder, with the test streams that ship
    // with the application. It is off by default: the chip gives up on work
    // it does not like, and that must not disturb the application.
    if (ProgramConfig::instance().getBoolOption(SettingItem::PS5_HWDEC_TEST)) {
        ps5decoder::Decoder::selfTest();
    }
#endif

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }
#ifdef PS5
    wiliwili_mark("application");
#endif

    // A watch over the main loop: a title that stops answering does not
    // write anything itself, so the watch says where it stopped.
    stage::startWatch();
    brls::Application::getRunLoopEvent()->subscribe([]() {
        stage::Marker marker("run loop");
        stage::tick();
    });

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow("wiliwili");
    brls::Logger::info("createWindow done");
#ifdef PS5
    wiliwili_mark("window");
#endif

    // Register custom view\theme\style
    Register::initCustomView();
    Register::initCustomTheme();
    Register::initCustomStyle();

    brls::Application::getPlatform()->disableScreenDimming(false);

    if (brls::Application::getPlatform()->isApplicationMode()) {
        Intent::openMain();
#ifdef PS5
        wiliwili_mark("main intent");
#endif
        // Uncomment these lines to debug activities
        //        Intent::openBV("BV1Da411Y7U4");  // 弹幕防遮挡 (横屏)
        //        Intent::openBV("BV1iN4y1m7J3");  // 弹幕防遮挡 (竖屏)
        //        Intent::openBV("BV1kT4y1s7od");  // 高级弹幕 测试0
        //        Intent::openBV("BV1eN4y147bC");  // 高级弹幕 测试1
        //        Intent::openBV("BV16x411D7NK");  // 高级弹幕 测试2
        //        Intent::openBV("BV1uW411e7gt");  // bas 弹幕 (Bilibili Animation Script)
        //        Intent::openBV("BV1zb4y1j7vz");  // flv 模式报错：HTTP 424
        //        Intent::openBV("BV1jL41167ZG");  // 充电视频
        //        Intent::openBV("BV1dx411c7Av");  // flv拼接视频
        //        Intent::openBV("BV15z4y1Z734");  // 4K HDR 视频
        //        Intent::openBV("BV1qM4y1w716");  // 8K
        //        Intent::openBV("BV1PN4y1G7u2");  // up主视频自动跳转番剧
        //        Intent::openBV("BV1sK411s7zq");  // 多P视频测试
        //        Intent::openBV("BV1Cg411j76F");  // 多字幕测试
        //        Intent::openBV("BV1A44y1u7PF");  // 测试FFMPEG在switch上的bug（加载时间过长）
        //        Intent::openBV("BV1eD4y1b7Jv");  // 测试 MPV 在switch上的bug（长时间播放崩溃）
        //        Intent::openBV("BV1U3411c7Qx");  // 测试长标题
        //        Intent::openBV("BV1fG411W7Px");  // 测试弹幕
        //        Intent::openSeasonByEpId(323434);// 测试电影
        //        Intent::openLive(1942240);       // 测试直播
        //        Intent::openSearch("harry");     // 测试搜索影片
        //        Intent::openTVSearch();          // 测试TV搜索模式
        //        Intent::openHint();              // 应用开启教程页面
        //        Intent::openCollection("2511565362"); // 测试打开收藏夹
        //        Intent::openPgcFilter("/page/home/pgc/more?type=2&index_type=2&area=2&order=2&season_status=-1&season_status=3,6"); // 影片分类索引
        //        Intent::openSetting();  //  设置页面
    } else {
        Intent::openHint();
    }

    GA("open_app", {{"version", APPVersion::instance().getVersionStr()},
                    {"language", brls::Application::getLocale()},
                    {"window", fmt::format("{}x{}", brls::Application::windowWidth, brls::Application::windowHeight)}})
    APPVersion::instance().checkUpdate();

    // Run the app
    // brls::Application::setLimitedFPS(60);
    while (brls::Application::mainLoop()) {
    }

    brls::Logger::info("mainLoop done");

    // Cleanup curl and Check whether restart is required
    ProgramConfig::instance().exit(argv);

#ifdef PS5
    // The console unloads a title through its system service. Without this
    // call the title's window stays on the screen and the console shows a
    // black frame instead of the home screen - the console's own dialog
    // asks whether to close the program, and then nothing happens.
    brls::Logger::info("leaving the title");
    sceSystemServiceLoadExec("exit", nullptr);
#endif

    // Exit
    return EXIT_SUCCESS;
}

#ifdef __WINRT__
#include <borealis/core/main.hpp>
#endif
