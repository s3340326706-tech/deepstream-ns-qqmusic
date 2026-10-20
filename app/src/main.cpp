// main.cpp —— Deepstream 入口
#include <borealis.hpp>

#include <cstdlib>

#include "player_activity.hpp"

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }

    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("Deepstream");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

    // 不让 + 直接退出：音乐播放器里 + 应该是别的功能，退出走 Home
    brls::Application::setGlobalQuit(false);

    brls::Application::pushActivity(new PlayerActivity());

    while (brls::Application::mainLoop())
        ;

    return EXIT_SUCCESS;
}
