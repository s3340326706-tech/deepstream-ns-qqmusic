// source_engine.hpp —— 音源配置引擎（C++ 移植版）
// 与 Python 参考实现 engine/source_engine.py 行为对齐；移植时改这里，不要改配置。
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "se/http.hpp"

namespace se {

using json = nlohmann::json;

struct Track {
    std::string id, title, artist, album, cover;
    long long duration_ms = 0;
    std::map<std::string, std::string> extra;
};

// 歌单（配置里的 playlists 段产出的条目）
struct Playlist {
    std::string id, name, cover, sub;
    int song_count = 0;
};

struct StreamResult {
    bool ok = false;
    std::string url, format, quality, error;
    long long expires_at = 0;  // 毫秒时间戳，0 表示未知
};

struct Diag {
    std::string quality, reason;
};

class SourceError : public std::runtime_error {
public:
    explicit SourceError(const std::string& m) : std::runtime_error(m) {}
};

class RequestError : public std::runtime_error {
public:
    explicit RequestError(const std::string& m) : std::runtime_error(m) {}
};

struct EngineOptions {
    bool insecure = false;
    std::string cainfo;     // Switch: "romfs:/cacert.pem"
    long timeout_sec = 8;
    size_t max_body = 4u * 1024u * 1024u;
};

class Engine {
public:
    // 读入 + 校验；不过就抛 SourceError
    static json load_file(const std::string& path);
    static void validate(const json& src);

    Engine() = default;
    explicit Engine(json src) : src_(std::move(src)) {}

    void set_cookies(std::map<std::string, std::string> c) { cookies_ = std::move(c); }
    const std::map<std::string, std::string>& cookies() const { return cookies_; }

    static std::map<std::string, std::string> parse_cookie_string(const std::string& s);
    static std::map<std::string, std::string> load_account(const std::string& path);

    const json& source() const { return src_; }
    std::string id() const;
    std::string name() const;
    std::vector<std::string> allowed_hosts() const;
    std::vector<std::string> encrypted_markers() const;
    std::vector<std::string> ticket_keys() const;

    std::vector<Track> search(const std::string& keyword, int page = 1);

    // 账号歌单。配置没写 playlists / playlist_songs 时返回空 / 抛 SourceError，
    // 这样不支持歌单的音源不会让界面崩掉。
    bool has_playlists() const;
    std::vector<Playlist> playlists();
    std::vector<Track> playlist_songs(const std::string& playlist_id);
    StreamResult resolve(const Track& t, const std::string& quality_pref = "",
                         std::vector<Diag>* diag = nullptr);
    std::string lyric(const Track& t);

    // 调试用：把某个操作的原始响应留下（对应 probe --tree / --dump）
    json raw_call(const std::string& op, const std::string& keyword, const Track& t);
    const json& last_response() const { return last_resp_; }

    EngineOptions opts;

private:
    // 执行一个「request → list」型操作，返回逐项 JSON（映射由调用方做）
    std::vector<json> fetch_list(const json& spec, const json& args);
    // 把一个 list 型 spec 按 map 映射成 Track（search / playlist_songs 共用）
    std::vector<Track> map_tracks(const json& spec, const std::vector<json>& items);

    json src_;
    std::map<std::string, std::string> cookies_;
    json last_resp_;
};

// 工具：从 URL 取 host（小写）
std::string host_of(const std::string& url);

}  // namespace se
