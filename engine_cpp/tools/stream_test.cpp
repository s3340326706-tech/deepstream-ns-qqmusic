// stream_test.cpp —— PC 侧验证「边下边播」整条链路
//
// 跑法与在 Switch 上完全一致：AudioEngine::load() → play() → 轮询 position()。
// PC 分支的 pump 按 48kHz 实时速度消费环形缓冲，所以背压 / EOF / seek 都会真实发生。
//
// 用法：
//   stream_test.exe <keyword>            搜第一首并播
//   stream_test.exe --url <audio_url>    直接播一个直链（跳过搜索）
//   stream_test.exe <keyword> --secs 40  最多盯 40 秒（默认整首）

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "player/audio_engine.hpp"
#include "se/source_engine.hpp"

static const char* kSourcePath = "src-private/sources/qqmusic.json";
static const char* kAccountPath = "src-private/account.json";

int main(int argc, char** argv) {
    std::string keyword;
    std::string direct_url;
    int max_secs = 0;
    std::string quality;
    bool list_playlists = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) direct_url = argv[++i];
        else if (std::strcmp(argv[i], "--secs") == 0 && i + 1 < argc) max_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--q") == 0 && i + 1 < argc) quality = argv[++i];
        else if (std::strcmp(argv[i], "--playlists") == 0) list_playlists = true;
        else keyword = argv[i];
    }

    std::string url = direct_url;

    // —— 歌单自检：不进播放链路，只看能不能把账号歌单和歌单内歌曲解出来 ——
    if (list_playlists) {
        se::Engine engine = [&]() {
            se::json src = se::Engine::load_file(kSourcePath);
            se::Engine::validate(src);
            se::Engine eng(src);
            eng.set_cookies(se::Engine::load_account(kAccountPath));
            return eng;
        }();

        printf("[A] playlists ...\n");
        auto pls = engine.playlists();
        printf("    -> %zu 个\n", pls.size());
        for (size_t i = 0; i < pls.size(); ++i) {
            printf("    %2zu. [%s] %s  (%d 首)  cover=%s\n", i + 1, pls[i].id.c_str(),
                   pls[i].name.c_str(), pls[i].song_count,
                   pls[i].cover.empty() ? "-" : pls[i].cover.c_str());
        }
        if (pls.empty()) return 1;

        const se::Playlist& first = pls[0];
        printf("\n[B] songs of \"%s\" ...\n", first.name.c_str());
        auto songs = engine.playlist_songs(first.id);
        printf("    -> %zu 首（歌单声称 %d 首）\n", songs.size(), first.song_count);
        for (size_t i = 0; i < songs.size() && i < 3; ++i) {
            printf("    %zu. %s | %s | %lldms | id=%s\n", i + 1, songs[i].title.c_str(),
                   songs[i].artist.c_str(), songs[i].duration_ms, songs[i].id.c_str());
            printf("       cover=%s\n", songs[i].cover.c_str());
        }

        if (!songs.empty()) {
            printf("\n[C] resolve first song ...\n");
            auto r = engine.resolve(songs[0], "320");
            printf("    ok=%d quality=%s format=%s\n", (int)r.ok, r.quality.c_str(),
                   r.format.c_str());
            printf("    url=%.110s\n", r.url.c_str());
        }
        return 0;
    }

    if (url.empty()) {
        // —— 1. 搜索 + 取流（和 app 里一样的调用顺序）——
        se::Engine engine = [&]() {
            se::json src = se::Engine::load_file(kSourcePath);
            se::Engine::validate(src);
            se::Engine eng(src);
            eng.set_cookies(se::Engine::load_account(kAccountPath));
            return eng;
        }();

        printf("[1/3] search \"%s\" ...\n", keyword.c_str());
        std::vector<se::Track> tracks = engine.search(keyword, 1);
        if (tracks.empty()) {
            printf("no result\n");
            return 1;
        }
        const se::Track& t = tracks[0];
        printf("      -> %s | %s | %dms\n", t.title.c_str(), t.artist.c_str(), t.duration_ms);
        printf("      cover: %s\n", t.cover.empty() ? "(none)" : t.cover.c_str());

        auto r = engine.resolve(t, quality);
        if (!r.ok) {
            printf("resolve failed: %s\n", r.error.c_str());
            return 1;
        }
        url = r.url;
    }

    printf("[2/3] url: %s\n", url.c_str());

    // —— 2. 打开流（只读头部，不下载整首）——
    auto& ae = hd::AudioEngine::get();
    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    if (!ae.load(url, &err)) {
        printf("load failed: %s\n", err.c_str());
        return 1;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    auto info = ae.info();
    printf("      codec=%s  %dHz/%dch  dur=%.1fs  head=%lldms  can_seek=%d\n",
           info.codec.c_str(), info.sample_rate, info.channels, ae.duration(), (long long)ms,
           (int)ae.can_seek());

    // —— 3. 播放（PC 上用实时消费模拟声卡）——
    ae.play();
    printf("[3/3] playing ...\n");

    bool seeked = false;
    int last_printed = -1;
    const int limit = max_secs > 0 ? max_secs : (int)(ae.duration() > 0 ? ae.duration() + 5 : 300);

    for (int s = 0; s < limit; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        double pos = ae.position();
        double dur = ae.duration();
        float prog = ae.load_progress();

        if ((int)pos != last_printed) {
            printf("      %3.0fs / %3.0fs   downloaded %3.0f%%   state=%d\n", pos, dur,
                   prog * 100.0f, (int)ae.state());
            last_printed = (int)pos;
        }

        // 中段试一次 seek（只有 FLAC 支持），验证 seek 回调 + 背压不卡死
        if (!seeked && dur > 20 && pos > 8) {
            seeked = true;
            printf("      -> seek to %.1fs\n", dur * 0.5);
            ae.seek(dur * 0.5);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            printf("      <- after seek: %.1fs\n", ae.position());
        }

        if (ae.state() == hd::PlayState::Failed) {
            printf("FAILED: %s\n", ae.last_error().c_str());
            return 1;
        }
        // 下载完 + 播到尾巴 = 结束
        if (dur > 0 && pos >= dur - 1.0 && prog >= 0.999f) {
            printf("      done at %.1fs (%.0f%% downloaded)\n", pos, prog * 100.0f);
            break;
        }
    }

    printf("[ok] unloading (释放内存缓冲，不落盘)\n");
    ae.unload();
    printf("[ok] state after unload = %d, pos=%.1f, dur=%.1f\n", (int)ae.state(), ae.position(),
           ae.duration());
    return 0;
}
