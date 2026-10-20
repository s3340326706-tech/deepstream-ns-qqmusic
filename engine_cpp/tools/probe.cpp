// probe.cpp —— 调试台（对应 Python 的 probe.py，行为尽量一致）
//
// PC:    g++ -std=c++17 -Iinclude -Isrc tools/probe.cpp src/*.cpp -lcurl -o probe
// Switch: 见 Makefile.switch
//
// 命令：
//   probe selftest
//   probe validate <source.json>
//   probe search   <source.json> "关键词"
//   probe resolve  <source.json> --mid <> [--media-mid <>] [--quality flac] [--bytes]
//   probe lyric    <source.json> --mid <>
//   probe flow     <source.json> "关键词" [--limit 3] [--bytes]
//   probe raw      <source.json> --op stream --mid <> [--tree]

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "se/source_engine.hpp"
#include "se/util.hpp"

#ifdef __SWITCH__
#include <switch.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define CA_PATH "romfs:/cacert.pem"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

using namespace se;

namespace {

// Windows 控制台把命令行按系统代码页（简体中文 = GBK）交给 main，
// 直接用会让中文关键词变成 GBK 字节、URL 编码后服务端返回非 UTF-8 的 JSON。
// 这里统一转成 UTF-8，和 Switch 侧（UI 原生 UTF-8）保持一致。
#ifdef _WIN32
std::string acp_to_utf8(const char* s) {
    int wl = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (wl <= 1) return std::string();
    std::wstring w((size_t)wl - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], wl);
    int ul = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ul <= 1) return std::string();
    std::string u((size_t)ul - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], ul, nullptr, nullptr);
    return u;
}
#endif

// console 只有 8x8 ASCII 位图字体：一个汉字占 3 字节，会被画成 3 个乱码方块，
// 而且把 80 列的窗口撑爆、整屏错行。这里统一把非 ASCII 码点折成单个 '?'，
// 保证任何一行都不会超宽。中文留给正式 GUI（borealis + 自带 CJK 字体）去显示。
void pout(const char* fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

#ifdef __SWITCH__
    std::string s;
    s.reserve(256);
    for (size_t i = 0; buf[i];) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) {
            s.push_back((char)c);
            ++i;
            continue;
        }
        // UTF-8 首字节判长度，整段折成一个 '?'
        size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        s.push_back('?');
        i += n;
    }
    fputs(s.c_str(), stdout);
#else
    // PC（Git Bash / 终端）本来就吃 UTF-8，保留原文，方便在电脑上看中文歌名
    fputs(buf, stdout);
#endif
}

#define printf pout

struct Args {
    std::string cmd, source, keyword, mid, media_mid, quality, op = "stream", account, cookie;
    int limit = 3, page = 1;
    bool bytes = false, tree = false, insecure = false;
};

Args parse(int argc, char** argv) {
    Args a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (s == "--mid") a.mid = next();
        else if (s == "--media-mid") a.media_mid = next();
        else if (s == "--quality") a.quality = next();
        else if (s == "--account") a.account = next();
        else if (s == "--cookie") a.cookie = next();
        else if (s == "--op") a.op = next();
        else if (s == "--limit") a.limit = std::atoi(next().c_str());
        else if (s == "--page") a.page = std::atoi(next().c_str());
        else if (s == "--bytes") a.bytes = true;
        else if (s == "--tree") a.tree = true;
        else if (s == "--insecure") a.insecure = true;
        else if (s.rfind("--", 0) == 0) { /* 忽略未知开关 */ }
        else pos.push_back(s);
    }
    if (!pos.empty()) a.cmd = pos[0];
    if (pos.size() > 1) a.source = pos[1];
    if (pos.size() > 2) a.keyword = pos[2];
    return a;
}

void print_tree(const json& j, int depth = 0, const std::string& prefix = "") {
    if (depth > 6) return;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string p = prefix.empty() ? it.key() : prefix + "." + it.key();
            print_tree(it.value(), depth + 1, p);
        }
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size() && i < 3; ++i)
            print_tree(j[i], depth + 1, prefix + "[" + std::to_string(i) + "]");
        if (j.size() > 3) printf("    %s  ... (%zu items)\n", prefix.c_str(), j.size());
    } else {
        printf("    %-52s %s\n", prefix.c_str(), j.dump().c_str());
    }
}

int selftest() {
    struct Case { const char* name; std::string got, want; };
    std::vector<Case> cases = {
        {"md5(abc)", md5_hex("abc"), "900150983cd24fb0d6963f7d28e17f72"},
        {"sha1(abc)", sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"base64(abc)", base64_encode("abc"), "YWJj"},
        {"url_encode(a b&c)", url_encode("a b&c"), "a%20b%26c"},
        {"digits(o123x)", digits_only("o123x"), "123"},
        {"host_of", host_of("https://u.y.qq.com/cgi-bin/musicu.fcg?a=1"), "u.y.qq.com"},
    };
    int fail = 0;
    for (const auto& c : cases) {
        bool ok = c.got == c.want;
        printf("  %-22s %s  %s\n", c.name, ok ? "OK  " : "FAIL", c.got.c_str());
        if (!ok) { printf("  %-22s      want %s\n", "", c.want.c_str()); ++fail; }
    }

    unsigned h33 = hash33("abc");
    printf("  %-22s %s  %u\n", "hash33(abc)", h33 == 108966u ? "OK  " : "FAIL", h33);
    if (h33 != 108966u) ++fail;

    unsigned g0 = gtk_of("");
    printf("  %-22s %s  %u\n", "gtk(\"\")", g0 == 5381u ? "OK  " : "FAIL", g0);
    if (g0 != 5381u) ++fail;

    printf("\n%d failed\n", fail);
    return fail == 0 ? 0 : 1;
}

void print_track(const Track& t) {
    printf("  [%s] %s - %s (%s)  %lld ms\n", t.id.c_str(), t.title.c_str(),
           t.artist.c_str(), t.album.c_str(), t.duration_ms);
}

bool probe_bytes(const std::string& url) {
    HttpRequest r;
    r.url = url;
    r.headers.push_back({"Range", "bytes=0-4095"});
    r.headers.push_back({"Referer", "https://y.qq.com/"});
    r.headers.push_back({"Origin", "https://y.qq.com"});
#ifdef __SWITCH__
    r.cainfo = CA_PATH;
#endif
    HttpResponse resp = http_perform(r);
    if (!resp.ok()) {
        printf("    probe failed: %s\n", resp.error.c_str());
        return false;
    }
    std::string magic = resp.body.substr(0, 4);
    const char* kind = "unknown";
    if (magic == "fLaC") kind = "FLAC";
    else if (magic.rfind("ID3", 0) == 0) kind = "MP3(ID3)";
    else if ((unsigned char)magic[0] == 0xFF) kind = "MP3";
    else if (magic.rfind("OggS", 0) == 0) kind = "OGG";
    else if (magic.find("ftyp") != std::string::npos) kind = "M4A";
    printf("    probe: status=%ld type=%s %zu bytes magic=%s -> %s\n", resp.status,
           resp.content_type.c_str(), resp.body.size(), magic.c_str(), kind);
    return resp.status == 200 || resp.status == 206;
}

#ifdef __SWITCH__
// 等 nifm 报"已连接"再放行，最多 10 秒。
// hbmenu 冷启动时 WiFi 往往还没关联完，直接发请求就会得到
// "Could not resolve host"，看起来像代码错，其实是时机问题。
void switch_net_wait() {
    NifmInternetConnectionType type = NifmInternetConnectionType_WiFi;
    u32 strength = 0;
    NifmInternetConnectionStatus st = NifmInternetConnectionStatus_ConnectingUnknown1;
    int waited_ms = 0;
    for (int i = 0; i < 50; ++i) {
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st)) &&
            st == NifmInternetConnectionStatus_Connected)
            break;
        svcSleepThread(200000000ULL);  // 200ms
        waited_ms += 200;
    }

    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    Result rc = nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2);
    char ip_s[32] = "?", dns_s[32] = "?";
    if (R_SUCCEEDED(rc)) {
        struct in_addr a;
        a.s_addr = ip;
        inet_ntop(AF_INET, &a, ip_s, sizeof(ip_s));
        a.s_addr = dns1;
        inet_ntop(AF_INET, &a, dns_s, sizeof(dns_s));
    }
    printf("[net] %s (%s, signal %d, waited %ds) ip=%s dns=%s\n",
           st == NifmInternetConnectionStatus_Connected ? "connected" : "DOWN",
           type == NifmInternetConnectionType_WiFi ? "wifi" : "ethernet",
           (int)strength, waited_ms / 1000, ip_s, dns_s);
}
#endif

}  // namespace

#ifdef __SWITCH__
static void wait_for_exit();  // 定义在文件尾部，selftest 分支也要用
#endif

int run_main(int argc, char** argv) {
#ifdef __SWITCH__
    consoleInit(nullptr);
    romfsInit();
    // 顺序不能反：bsd 服务启动时要向 nifm 取网络配置（含 DNS），
    // 必须先 init nifm 并等它真连上，否则 getaddrinfo 恒失败，
    // 报错长得跟"没联网"一样（curl: (6) Could not resolve host）。
    nifmInitialize(NifmServiceType_User);
    switch_net_wait();
    socketInitializeDefault();
#endif

    Args a = parse(argc, argv);
#ifdef _WIN32
    a.keyword = acp_to_utf8(a.keyword.c_str());
#endif
#ifdef __SWITCH__
    // hbmenu 不能传命令行参数：没给参数就跑内置回归（验证 TLS + 取流）
    if (argc < 2) {
        a.cmd = "flow";
        a.source = "sdmc:/switch/se_sources/qqmusic.json";
        a.account = "sdmc:/switch/se_sources/account.json";
        a.keyword = "晴天";
        a.limit = 2;
        a.bytes = true;
        printf("built-in regression (fixed CJK search term)\nsource: %s\n", a.source.c_str());
    }
#endif

    if (a.cmd == "selftest") {
        int rc = selftest();
#ifdef __SWITCH__
        wait_for_exit();
#endif
        return rc;
    }

    if (a.cmd.empty() || a.source.empty()) {
        printf("usage: probe <validate|search|resolve|lyric|flow|raw|selftest> <source.json> [options]\n");
        return 2;
    }

    json src;
    try {
        src = Engine::load_file(a.source);
    } catch (const std::exception& e) {
        printf("[config error] %s\n", e.what());
        return 2;
    }

    Engine engine(src);
#ifdef __SWITCH__
    engine.opts.cainfo = CA_PATH;
#endif
    engine.opts.insecure = a.insecure;

    if (a.cmd == "validate") {
        printf("source config OK\n");
        printf("  id          : %s\n", engine.id().c_str());
        printf("  name        : %s\n", engine.name().c_str());
        auto hosts = engine.allowed_hosts();
        printf("  allowed_hosts : %zu\n", hosts.size());
        for (const auto& h : hosts) printf("      - %s\n", h.c_str());
        auto marks = engine.encrypted_markers();
        printf("  encrypted_markers:");
        for (const auto& m : marks) printf(" %s", m.c_str());
        printf("\n");
        return 0;
    }

    // 凭据
    std::map<std::string, std::string> cookies;
    if (!a.account.empty()) cookies = Engine::load_account(a.account);
    if (!a.cookie.empty()) {
        auto parsed = Engine::parse_cookie_string(a.cookie);
        cookies.insert(parsed.begin(), parsed.end());
    }
    engine.set_cookies(cookies);
    if (!cookies.empty()) {
        printf("[cred] %zu fields loaded\n", cookies.size());
        auto tickets = engine.ticket_keys();
        bool hit = false;
        for (const auto& k : tickets)
            if (cookies.count(k) && !cookies[k].empty()) hit = true;
        if (!tickets.empty() && !hit) printf("[cred] WARN no login ticket matched - probably not logged in\n");
        if (!cookies.count("pgv_pvid") || cookies["pgv_pvid"].empty())
            printf("[cred] WARN no pgv_pvid - purl will be empty (it IS the guid)\n");
    }

    try {
        if (a.cmd == "search") {
            if (a.keyword.empty()) { printf("missing keyword\n"); return 2; }
            auto list = engine.search(a.keyword, a.page);
            printf("hits: %zu\n", list.size());
            for (int i = 0; i < a.limit && i < (int)list.size(); ++i) print_track(list[i]);
            if (a.tree) print_tree(engine.last_response());
            return 0;
        }

        if (a.cmd == "resolve") {
            if (a.mid.empty()) { printf("missing --mid\n"); return 2; }
            Track t;
            t.id = a.mid;
            if (!a.media_mid.empty()) t.extra["media_mid"] = a.media_mid;
            std::vector<Diag> diag;
            auto r = engine.resolve(t, a.quality, &diag);
            for (const auto& d : diag) printf("  [%s] %s\n", d.quality.c_str(), d.reason.c_str());
            if (r.ok) {
                printf("  ok      : true\n  quality : %s\n  format  : %s\n  expires : %lld\n  url     : %s\n",
                       r.quality.c_str(), r.format.c_str(), r.expires_at, r.url.c_str());
                if (a.bytes) probe_bytes(r.url);
            } else {
                printf("  ok      : false\n  error   : %s\n", r.error.c_str());
                return 1;
            }
            if (a.tree) print_tree(engine.last_response());
            return 0;
        }

        if (a.cmd == "lyric") {
            if (a.mid.empty()) { printf("missing --mid\n"); return 2; }
            Track t;
            t.id = a.mid;
            std::string text = engine.lyric(t);
            printf("lyric %zu bytes\n", text.size());
#ifndef __SWITCH__
            // 歌词是中文，控制台画不出来，Switch 上只看字节数就够了
            printf("%s\n", text.substr(0, 600).c_str());
#endif
            if (a.tree) print_tree(engine.last_response());
            return 0;
        }

        if (a.cmd == "flow") {
            if (a.keyword.empty()) { printf("missing keyword\n"); return 2; }
            auto list = engine.search(a.keyword, a.page);
            printf("hits: %zu, resolving first %d\n", list.size(), a.limit);
            int ok = 0;
            for (int i = 0; i < a.limit && i < (int)list.size(); ++i) {
                const Track& t = list[i];
                std::vector<Diag> diag;
                auto r = engine.resolve(t, a.quality, &diag);
                printf("  %s [%s] %s - %s\n", r.ok ? "OK " : "FAIL", t.id.c_str(),
                       t.title.c_str(), t.artist.c_str());
                for (const auto& d : diag)
                    printf("        [%s] %s\n", d.quality.c_str(), d.reason.c_str());
                if (r.ok) {
                    ++ok;
                    printf("        -> %s (%s)\n", r.url.substr(0, 100).c_str(), r.format.c_str());
                    if (a.bytes) probe_bytes(r.url);
                }
            }
            printf("OK %d / %d\n", ok, std::min((int)list.size(), a.limit));
            return ok > 0 ? 0 : 1;
        }

        if (a.cmd == "raw") {
            Track t;
            t.id = a.mid;
            if (!a.media_mid.empty()) t.extra["media_mid"] = a.media_mid;
            json resp = engine.raw_call(a.op, a.keyword, t);
            if (a.tree) print_tree(resp);
            else printf("%s\n", resp.dump(2).c_str());
            return 0;
        }

    } catch (const std::exception& e) {
        printf("[fail] %s\n", e.what());
        return 1;
    }

    printf("unknown command %s\n", a.cmd.c_str());
    return 2;
}

// Switch 上命令跑完直接 return 会瞬间退出，屏幕上的结果根本看不清。
// 统一在外面等一次「+」，让所有分支都能停在结果页。
//
// 注意：不能用 padGetButtonsDown 的边沿判断——hbmenu 是 applet 模式，
// 边沿检测依赖"两次 padUpdate 之间状态发生跳变"，很容易被吞掉。
// 这里改成「持续按住 N 帧」才算数，按不死也能退。
#ifdef __SWITCH__
static void wait_for_exit() {
    printf("\nPress + (or B) to exit\n");
    consoleUpdate(nullptr);

    PadState pad;
    padInitializeDefault(&pad);
    int plus_frames = 0, b_frames = 0;
    unsigned frame = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad);

        plus_frames = (held & HidNpadButton_Plus) ? plus_frames + 1 : 0;
        b_frames    = (held & HidNpadButton_B)    ? b_frames + 1    : 0;
        if (plus_frames >= 3 || b_frames >= 3) break;

        // 每 60 帧刷一次状态：能看到按键位就说明 hid 通了，
        // 一直是 0 说明输入根本没进到程序里（那是另一类问题）。
        if (frame % 60 == 0)
            printf("\r[exit] keys=0x%04llx  waiting...   ",
                   (unsigned long long)(held & 0xFFFF));
        ++frame;
        consoleUpdate(nullptr);
        svcSleepThread(16000000ULL);  // 16ms ≈ 60fps，别空转烧 CPU
    }
    printf("\n[exit] bye\n");
    consoleUpdate(nullptr);
}
#endif

int main(int argc, char** argv) {
#ifdef __SWITCH__
    int rc = run_main(argc, argv);
    wait_for_exit();
    // 逐步打日志：万一某一步挂住，屏幕上能看出停在哪
    printf("cleanup: socketExit\n");   consoleUpdate(nullptr); socketExit();
    printf("cleanup: nifmExit\n");     consoleUpdate(nullptr); nifmExit();
    printf("cleanup: romfsExit\n");    consoleUpdate(nullptr); romfsExit();
    consoleExit(nullptr);
    return rc;
#else
    return run_main(argc, argv);
#endif
}
