#include "se/source_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#include "se/util.hpp"

namespace se {
namespace {

const char* kDefaultUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

const size_t kMaxList = 200;
const size_t kMaxAttempts = 8;
const size_t kMaxFileBytes = 256u * 1024u;

// 加密流标记：配置只能追加，不能清空（fail-closed）
const std::vector<std::string> kBaseEncrypted = {".mflac", ".mgg", ".mflac0",
                                                 ".mgg1", ".mmp4"};

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string rand16() {
    static std::mt19937 rng((unsigned)now_ms());
    std::uniform_int_distribution<int> d(0, 15);
    static const char* H = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) s.push_back(H[d(rng)]);
    return s;
}

const json* jget(const json& o, const std::string& k) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(k);
    return it == o.end() ? nullptr : &(*it);
}

std::string jstr(const json& v) {
    if (v.is_null()) return "";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        std::ostringstream o;
        o << v.get<double>();
        return o.str();
    }
    return v.dump();
}

long long jint(const json& v) {
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number_unsigned()) return (long long)v.get<unsigned long long>();
    if (v.is_number_float()) return (long long)v.get<double>();
    std::string d = digits_only(jstr(v));
    if (d.empty()) return 0;
    return std::atoll(d.c_str());
}

json parse_body(const std::string& raw) {
    // 先直接解；失败再跳过 JSONP 前缀重试
    std::string err1;
    try {
        return json::parse(raw);
    } catch (const std::exception& e) {
        err1 = e.what();
    }
    size_t p = raw.find_first_of("{[");
    if (p != std::string::npos) {
        try {
            return json::parse(raw.substr(p));
        } catch (...) {
        }
    }
    char head[81] = {0};
    size_t n = raw.size() < 80 ? raw.size() : 80;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)raw[i];
        head[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    throw RequestError("response is not JSON (" + std::to_string(raw.size()) + " bytes, head: " + std::string(head) +
                       "; parse error: " + err1 + ")");
}

// "req_0.data.midurlinfo[0].purl" 式取值；缺键返回 null
json walk(const json& root, const std::string& path) {
    if (path.empty()) return root;
    const json* cur = &root;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t dot = path.find('.', pos);
        std::string seg = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);

        size_t lb = seg.find('[');
        std::string name = (lb == std::string::npos) ? seg : seg.substr(0, lb);
        if (!name.empty()) {
            if (!cur->is_object()) return json();
            auto it = cur->find(name);
            if (it == cur->end()) return json();
            cur = &(*it);
        }
        size_t p = (lb == std::string::npos) ? std::string::npos : lb;
        while (p != std::string::npos && p < seg.size()) {
            size_t l = seg.find('[', p);
            size_t r = seg.find(']', p);
            if (l == std::string::npos || r == std::string::npos) break;
            int idx = std::atoi(seg.substr(l + 1, r - l - 1).c_str());
            if (!cur->is_array() || idx < 0 || idx >= (int)cur->size()) return json();
            cur = &((*cur)[(size_t)idx]);
            p = r + 1;
        }
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return *cur;
}

struct Ctx {
    const json* src = nullptr;
    const std::map<std::string, std::string>* cookies = nullptr;
    json args = json::object();
    json track = json::object();
    json item;
    json resp;
    std::string host;
    bool cookie_allowed = true;
    std::map<std::string, json> var_cache;
    std::vector<std::string> var_stack;
};

json eval(const json& node, Ctx& ctx);
json lookup(const std::string& expr, Ctx& ctx);

json resolve_var(const std::string& name, Ctx& ctx) {
    auto hit = ctx.var_cache.find(name);
    if (hit != ctx.var_cache.end()) return hit->second;
    for (const auto& s : ctx.var_stack)
        if (s == name) throw SourceError("vars cycle: " + name);
    const json* defs = jget(*ctx.src, "vars");
    const json* def = defs ? jget(*defs, name) : nullptr;
    if (!def) return json();
    ctx.var_stack.push_back(name);
    json val;
    try {
        val = eval(*def, ctx);
    } catch (...) {
        ctx.var_stack.pop_back();
        throw;
    }
    ctx.var_stack.pop_back();
    ctx.var_cache[name] = val;
    return val;
}

json call_fn(const std::string& name, const std::vector<json>& a) {
    auto arg = [&](size_t i) -> json { return i < a.size() ? a[i] : json(); };
    if (name == "hash33") return (unsigned)hash33(jstr(arg(0)));
    if (name == "gtk") return (unsigned)gtk_of(jstr(arg(0)));
    if (name == "md5") return md5_hex(jstr(arg(0)));
    if (name == "sha1") return sha1_hex(jstr(arg(0)));
    if (name == "base64") return base64_encode(jstr(arg(0)));
    if (name == "urlencode") return url_encode(jstr(arg(0)));
    if (name == "digits") return digits_only(jstr(arg(0)));
    if (name == "concat") {
        std::string s;
        for (const auto& x : a) s += jstr(x);
        return s;
    }
    if (name == "substr") {
        std::string s = jstr(arg(0));
        long long from = a.size() > 1 ? jint(a[1]) : 0;
        long long to = a.size() > 2 ? jint(a[2]) : (long long)s.size();
        if (from < 0) from = 0;
        if (to > (long long)s.size()) to = (long long)s.size();
        if (to < from) return std::string();
        return s.substr((size_t)from, (size_t)(to - from));
    }
    if (name == "default") {
        json v = arg(0);
        json fb = arg(1);
        bool empty = v.is_null() || (v.is_string() && v.get<std::string>().empty()) ||
                     (v.is_array() && v.empty()) || (v.is_object() && v.empty());
        return empty ? fb : v;
    }
    if (name == "int") return jint(arg(0));
    if (name == "mul") {
        double out = 1.0;
        if (a.empty()) return 0;
        for (const auto& x : a) out *= jstr(x).empty() ? 0.0 : std::atof(jstr(x).c_str());
        if (std::fabs(out - std::floor(out)) < 1e-9) return (long long)out;
        return out;
    }
    if (name == "json") {
        try {
            return json::parse(jstr(arg(0)));
        } catch (...) {
            return json();
        }
    }
    throw SourceError("unknown function " + name + " (not in whitelist)");
}

json lookup(const std::string& expr, Ctx& ctx) {
    if (expr == "$now") return now_ms();
    if (expr == "$rand16") return rand16();

    size_t dot = expr.find('.');
    std::string head = (dot == std::string::npos) ? expr.substr(1) : expr.substr(1, dot - 1);
    std::string rest = (dot == std::string::npos) ? "" : expr.substr(dot + 1);

    if (head == "arg") return walk(ctx.args, rest);
    if (head == "track") return walk(ctx.track, rest);
    if (head == "item") return walk(ctx.item, rest);
    if (head == "resp") return walk(ctx.resp, rest);
    if (head == "cookie") {
        if (!ctx.cookie_allowed)
            throw SourceError("refusing to send cookie to " + ctx.host + " (not in allowed_hosts)");
        if (ctx.cookies) {
            auto it = ctx.cookies->find(rest);
            if (it != ctx.cookies->end()) return it->second;
        }
        return json();
    }
    if (head == "var") return resolve_var(rest, ctx);
    throw SourceError("unknown expression namespace: " + expr);
}

json eval(const json& node, Ctx& ctx) {
    if (node.is_object()) {
        auto fn = node.find("fn");
        if (fn != node.end() && fn->is_string()) {
            std::vector<json> args;
            auto it = node.find("args");
            if (it != node.end() && it->is_array())
                for (const auto& x : *it) args.push_back(eval(x, ctx));
            return call_fn(fn->get<std::string>(), args);
        }
        json out = json::object();
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '_') continue;
            out[it.key()] = eval(it.value(), ctx);
        }
        return out;
    }
    if (node.is_array()) {
        json out = json::array();
        for (const auto& x : node) out.push_back(eval(x, ctx));
        return out;
    }
    if (node.is_string()) {
        const std::string s = node.get<std::string>();
        if (!s.empty() && s[0] == '$') return lookup(s, ctx);
    }
    return node;
}

std::string build_url(const json& req, Ctx& ctx) {
    json u = eval(req.at("url"), ctx);
    std::string url = jstr(u);

    const json* params = jget(req, "params");
    if (params && params->is_object()) {
        std::string qs;
        for (auto it = params->begin(); it != params->end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '_') continue;
            json v = eval(it.value(), ctx);
            if (v.is_null()) continue;
            std::string val = v.is_boolean() && !v.get<bool>() ? "" : jstr(v);
            if (!qs.empty()) qs += "&";
            qs += url_encode(it.key()) + "=" + url_encode(val);
        }
        if (!qs.empty()) url += (url.find('?') == std::string::npos ? "?" : "&") + qs;
    }
    return url;
}

json do_request(const json& src, Ctx& ctx, const json& req, const EngineOptions& opts) {
    if (!req.is_object()) throw RequestError("request structure missing");

    std::string url = build_url(req, ctx);
    std::string host = host_of(url);

    std::vector<std::string> allowed;
    if (const json* auth = jget(src, "auth"))
        if (const json* ah = jget(*auth, "allowed_hosts"))
            if (ah->is_array())
                for (const auto& h : *ah) allowed.push_back(to_lower(jstr(h)));

    bool ok_host = allowed.empty();
    for (const auto& h : allowed)
        if (h == host) ok_host = true;
    if (!ok_host) throw SourceError("request host " + host + " not in allowed_hosts");

    ctx.host = host;
    ctx.cookie_allowed = ok_host;

    HttpRequest hreq;
    hreq.url = url;
    hreq.timeout_sec = opts.timeout_sec;
    hreq.max_body = opts.max_body;
    hreq.insecure = opts.insecure;
    hreq.cainfo = opts.cainfo;

    if (const json* hs = jget(req, "headers")) {
        if (hs->is_object()) {
            for (auto it = hs->begin(); it != hs->end(); ++it) {
                if (!it.key().empty() && it.key()[0] == '_') continue;
                json v = eval(it.value(), ctx);
                if (!v.is_null()) hreq.headers.push_back({it.key(), jstr(v)});
            }
        }
    }
    bool has_ua = false;
    for (const auto& h : hreq.headers)
        if (to_lower(h.first) == "user-agent") has_ua = true;
    if (!has_ua) hreq.headers.push_back({"User-Agent", kDefaultUA});

    // Cookie：raw 模式整份带过去；keys 模式只带列出的键
    std::string cookie_fmt = "keys";
    std::vector<std::string> cookie_keys;
    if (const json* auth = jget(src, "auth")) {
        if (const json* cf = jget(*auth, "cookie_format")) cookie_fmt = jstr(*cf);
        if (const json* ck = jget(*auth, "cookie_keys"))
            if (ck->is_array())
                for (const auto& k : *ck) cookie_keys.push_back(jstr(k));
    }
    if (ctx.cookies && ctx.cookie_allowed) {
        std::string cookie_header;
        if (cookie_fmt == "raw") {
            for (const auto& kv : *ctx.cookies) {
                if (kv.second.empty()) continue;
                if (!cookie_header.empty()) cookie_header += "; ";
                cookie_header += kv.first + "=" + kv.second;
            }
        } else {
            for (const auto& k : cookie_keys) {
                auto it = ctx.cookies->find(k);
                if (it == ctx.cookies->end() || it->second.empty()) continue;
                if (!cookie_header.empty()) cookie_header += "; ";
                cookie_header += k + "=" + it->second;
            }
        }
        if (!cookie_header.empty()) hreq.headers.push_back({"Cookie", cookie_header});
    }

    if (const json* bj = jget(req, "body_json")) {
        json body = eval(*bj, ctx);
        hreq.body = body.dump(-1, ' ', false);
        hreq.has_body = true;
        hreq.method = "POST";
        bool has_ct = false;
        for (const auto& h : hreq.headers)
            if (to_lower(h.first) == "content-type") has_ct = true;
        if (!has_ct) hreq.headers.push_back({"Content-Type", "application/json"});
    } else if (const json* bf = jget(req, "body_form")) {
        json body = eval(*bf, ctx);
        std::string form;
        if (body.is_object()) {
            for (auto it = body.begin(); it != body.end(); ++it) {
                if (!it.key().empty() && it.key()[0] == '_') continue;
                if (!form.empty()) form += "&";
                form += url_encode(it.key()) + "=" + url_encode(jstr(it.value()));
            }
        }
        hreq.body = form;
        hreq.has_body = true;
        hreq.method = "POST";
        hreq.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    }

    if (const json* m = jget(req, "method")) {
        std::string mm = jstr(*m);
        if (!mm.empty()) hreq.method = mm;
    }

    HttpResponse resp = http_perform(hreq);
    if (!resp.ok()) throw RequestError(resp.error);
    return parse_body(resp.body);
}

std::string b64_decode(const std::string& in) {
    static int T[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)A[i]] = i;
        ready = true;
    }
    std::string out;
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int v = T[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string html_unescape(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(in[i++]);
            continue;
        }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() > 1 && ent[0] == '#') {
            long code = 0;
            if (ent[1] == 'x' || ent[1] == 'X')
                code = std::strtol(ent.c_str() + 2, nullptr, 16);
            else
                code = std::strtol(ent.c_str() + 1, nullptr, 10);
            if (code > 0 && code < 0x80) {
                out.push_back((char)code);
            } else if (code >= 0x80 && code < 0x800) {
                out.push_back((char)(0xC0 | (code >> 6)));
                out.push_back((char)(0x80 | (code & 0x3F)));
            } else if (code >= 0x800 && code < 0x10000) {
                out.push_back((char)(0xE0 | (code >> 12)));
                out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (code & 0x3F)));
            } else {
                out.push_back('?');
            }
        } else {
            out += in.substr(i, semi - i + 1);
        }
        i = semi + 1;
    }
    return out;
}

json track_to_json(const Track& t) {
    json j = json::object();
    j["id"] = t.id;
    j["title"] = t.title;
    j["artist"] = t.artist;
    j["album"] = t.album;
    j["cover"] = t.cover;
    j["duration_ms"] = t.duration_ms;
    json e = json::object();
    for (const auto& kv : t.extra) e[kv.first] = kv.second;
    j["extra"] = e;
    return j;
}

void collect_requests(const json& src,
                      std::vector<std::pair<std::string, const json*>>& out) {
    auto add = [&](const std::string& label, const json& holder) {
        auto r = holder.find("request");
        if (r != holder.end() && r->is_object()) out.push_back({label, &(*r)});
    };
    if (const json* s = jget(src, "search")) add("search.request", *s);
    if (const json* st = jget(src, "stream")) {
        if (const json* at = jget(*st, "attempts")) {
            int i = 0;
            for (const auto& x : *at) {
                if (x.is_object()) add("stream.attempts[" + std::to_string(i) + "].request", x);
                ++i;
            }
        }
    }
    // 其余「request → list」型操作（lyric / playlists / playlist_songs / 以后新增的）
    // 一律自动纳入校验。这里必须"发现式"收集，不能逐个列名字 ——
    // 漏掉一个就等于跳过 allowed_hosts 检查，Cookie 可能被发到任意主机。
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (!it.value().is_object()) continue;
        if (it.key() == "search" || it.key() == "stream") continue;
        if (it.value().find("request") != it.value().end()) add(it.key() + ".request", it.value());
    }
    if (const json* au = jget(src, "auth")) {
        if (const json* pl = jget(*au, "post_login")) {
            int i = 0;
            for (const auto& x : *pl) {
                if (x.is_object()) out.push_back({"auth.post_login[" + std::to_string(i) + "]", &x});
                ++i;
            }
        }
    }
}

}  // namespace

// --------------------------------------------------------------------------
// 公用
// --------------------------------------------------------------------------

std::string host_of(const std::string& url) {
    size_t p = url.find("://");
    size_t start = (p == std::string::npos) ? 0 : p + 3;
    size_t end = url.find_first_of(":/?#", start);
    return to_lower(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

json Engine::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw SourceError("file not found: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > kMaxFileBytes) throw SourceError("config file exceeds 256KB cap");
    json src;
    try {
        src = json::parse(ss.str());
    } catch (const std::exception& e) {
        throw SourceError(std::string("JSON syntax error: ") + e.what());
    }
    validate(src);
    return src;
}

void Engine::validate(const json& src) {
    if (!src.is_object()) throw SourceError("top level must be an object");

    const json* ver = jget(src, "api_version");
    if (!ver || !ver->is_number_integer()) throw SourceError("missing api_version");
    if (jint(*ver) > 1) throw SourceError("api_version higher than supported (1)");

    const json* idv = jget(src, "id");
    if (!idv || jstr(*idv).empty()) throw SourceError("missing required field: id");
    const json* namev = jget(src, "name");
    if (!namev || jstr(*namev).empty()) throw SourceError("missing required field: name");

    if (!jget(src, "search") && !jget(src, "stream") && !jget(src, "lyric"))
        throw SourceError("at least one of search / stream / lyric is required");

    if (const json* st = jget(src, "stream")) {
        if (const json* at = jget(*st, "attempts")) {
            if (at->is_array() && at->size() > kMaxAttempts)
                throw SourceError("stream.attempts allows at most 8 entries");
        }
    }

    const json* auth = jget(src, "auth");
    std::vector<std::string> allowed;
    bool need_cookie = false;
    if (auth) {
        if (const json* ah = jget(*auth, "allowed_hosts"))
            if (ah->is_array())
                for (const auto& h : *ah) allowed.push_back(to_lower(jstr(h)));
        if (const json* ck = jget(*auth, "cookie_keys"))
            if (ck->is_array() && !ck->empty()) need_cookie = true;
        if (const json* cf = jget(*auth, "cookie_format"))
            if (jstr(*cf) == "raw") need_cookie = true;
    }
    if (need_cookie && allowed.empty())
        throw SourceError("credentials declared but allowed_hosts missing (cookies could go to arbitrary hosts)");

    std::vector<std::pair<std::string, const json*>> reqs;
    collect_requests(src, reqs);
    for (const auto& kv : reqs) {
        const json* u = jget(*kv.second, "url");
        if (!u || !u->is_string()) throw SourceError(kv.first + ".url is not a string");
        const std::string url = u->get<std::string>();
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            throw SourceError(kv.first + ".url is not a valid http(s) url");
        const std::string h = host_of(url);
        if (!allowed.empty()) {
            bool hit = false;
            for (const auto& a : allowed)
                if (a == h) hit = true;
            if (!hit) throw SourceError(kv.first + " host " + h + " not in allowed_hosts");
        }
    }
}

std::map<std::string, std::string> Engine::parse_cookie_string(const std::string& s) {
    std::map<std::string, std::string> out;
    std::string norm = s;
    for (char& c : norm)
        if (c == '\n' || c == '\r') c = ';';
    for (const auto& part : split(norm, ';')) {
        std::string p = trim(part);
        if (p.empty()) continue;
        size_t eq = p.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(p.substr(0, eq));
        std::string v = trim(p.substr(eq + 1));
        if (!k.empty()) out[k] = v;
    }
    return out;
}

std::map<std::string, std::string> Engine::load_account(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    json j;
    try {
        j = json::parse(ss.str());
    } catch (...) {
        return {};
    }
    std::map<std::string, std::string> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.key().empty() && it.key()[0] == '_') continue;
        if (!it.value().is_string()) continue;
        if (it.key() == "cookie") {
            auto parsed = parse_cookie_string(it.value().get<std::string>());
            out.insert(parsed.begin(), parsed.end());
        } else {
            out[it.key()] = it.value().get<std::string>();
        }
    }
    return out;
}

std::string Engine::id() const { return jstr(jget(src_, "id") ? *jget(src_, "id") : json()); }
std::string Engine::name() const { return jstr(jget(src_, "name") ? *jget(src_, "name") : json()); }

std::vector<std::string> Engine::allowed_hosts() const {
    std::vector<std::string> out;
    if (const json* a = jget(src_, "auth"))
        if (const json* h = jget(*a, "allowed_hosts"))
            if (h->is_array())
                for (const auto& x : *h) out.push_back(jstr(x));
    return out;
}

std::vector<std::string> Engine::ticket_keys() const {
    std::vector<std::string> out;
    if (const json* a = jget(src_, "auth"))
        if (const json* h = jget(*a, "ticket_keys"))
            if (h->is_array())
                for (const auto& x : *h) out.push_back(jstr(x));
    return out;
}

std::vector<std::string> Engine::encrypted_markers() const {
    std::vector<std::string> out = kBaseEncrypted;
    if (const json* st = jget(src_, "stream"))
        if (const json* em = jget(*st, "encrypted_markers"))
            if (em->is_array()) {
                for (const auto& m : *em) {
                    std::string s = to_lower(jstr(m));
                    bool dup = false;
                    for (const auto& x : out)
                        if (x == s) dup = true;
                    if (!dup && !s.empty()) out.push_back(s);
                }
            }
    return out;
}

// --------------------------------------------------------------------------
// 三个操作
// --------------------------------------------------------------------------

// 「request → list」型操作的公共前半段。
// search / playlists / playlist_songs 都只是 request 不同、list 路径不同，
// 所以这里只负责拿到"逐项 JSON"，取字段的事交给 map_tracks / 各自映射。
std::vector<json> Engine::fetch_list(const json& spec, const json& args) {
    Ctx ctx;
    ctx.src = &src_;
    ctx.cookies = &cookies_;
    ctx.args = args;

    ctx.resp = do_request(src_, ctx, spec.at("request"), opts);
    last_resp_ = ctx.resp;

    json listing = ctx.resp;
    if (const json* lp = jget(spec, "list")) listing = eval(*lp, ctx);
    if (!listing.is_array()) {
        std::string snip = ctx.resp.is_null() ? std::string("<null>")
                                              : ctx.resp.dump().substr(0, 300);
        throw RequestError("list did not evaluate to an array; resp=" + snip);
    }

    std::vector<json> out;
    out.reserve(listing.size() < kMaxList ? listing.size() : kMaxList);
    for (const auto& elem : listing) {
        if (out.size() >= kMaxList) break;
        out.push_back(elem);
    }
    return out;
}

std::vector<Track> Engine::map_tracks(const json& spec, const std::vector<json>& items) {
    Ctx ctx;
    ctx.src = &src_;
    ctx.cookies = &cookies_;

    std::vector<Track> out;
    out.reserve(items.size());
    for (const auto& elem : items) {
        ctx.item = elem;

        Track t;
        if (const json* mp = jget(spec, "map")) {
            for (auto it = mp->begin(); it != mp->end(); ++it) {
                if (!it.key().empty() && it.key()[0] == '_') continue;
                json v = eval(it.value(), ctx);
                if (it.key() == "id") t.id = jstr(v);
                else if (it.key() == "title") t.title = jstr(v);
                else if (it.key() == "artist") t.artist = jstr(v);
                else if (it.key() == "album") t.album = jstr(v);
                else if (it.key() == "cover") t.cover = jstr(v);
                else if (it.key() == "duration_ms") t.duration_ms = jint(v);
                else if (it.key() == "extra" && v.is_object()) {
                    for (auto e = v.begin(); e != v.end(); ++e) {
                        if (!e.key().empty() && e.key()[0] == '_') continue;
                        t.extra[e.key()] = jstr(e.value());
                    }
                }
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Track> Engine::search(const std::string& keyword, int page) {
    const json* spec = jget(src_, "search");
    if (!spec) throw SourceError("source does not implement search");

    json args = json::object();
    args["keyword"] = keyword;
    args["page"] = page;
    return map_tracks(*spec, fetch_list(*spec, args));
}

// ── 歌单 ─────────────────────────────────────────────────────────────
bool Engine::has_playlists() const { return jget(src_, "playlists") != nullptr; }

std::vector<Playlist> Engine::playlists() {
    const json* spec = jget(src_, "playlists");
    if (!spec) throw SourceError("source does not implement playlists");

    // 歌单列表基本都要 uin + g_tk，具体参数由配置里的 params 决定
    const auto items = fetch_list(*spec, json::object());

    Ctx ctx;
    ctx.src = &src_;
    ctx.cookies = &cookies_;

    std::vector<Playlist> out;
    out.reserve(items.size());
    for (const auto& elem : items) {
        ctx.item = elem;
        Playlist p;
        if (const json* mp = jget(*spec, "map")) {
            for (auto it = mp->begin(); it != mp->end(); ++it) {
                if (!it.key().empty() && it.key()[0] == '_') continue;
                json v = eval(it.value(), ctx);
                if (it.key() == "id") p.id = jstr(v);
                else if (it.key() == "name") p.name = jstr(v);
                else if (it.key() == "cover") p.cover = jstr(v);
                else if (it.key() == "sub") p.sub = jstr(v);
                else if (it.key() == "song_count") p.song_count = (int)jint(v);
            }
        }
        if (p.id.empty() || p.id == "0") continue;   // id=0 是平台系统目录，里面没有可播放的歌
        if (p.name.empty()) p.name = "未命名歌单";
        // 有些歌单（如"我喜欢"）只给一个 "?n=1" 这样的相对后缀，没有 host。
        // 原样交给界面只会拿一个必败的 URL，不如直接当"没有封面"，界面会显示占位图标。
        if (p.cover.rfind("http", 0) != 0) p.cover.clear();
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Track> Engine::playlist_songs(const std::string& playlist_id) {
    const json* spec = jget(src_, "playlist_songs");
    if (!spec) throw SourceError("source does not implement playlist_songs");

    json args = json::object();
    args["playlist_id"] = playlist_id;
    return map_tracks(*spec, fetch_list(*spec, args));
}


StreamResult Engine::resolve(const Track& t, const std::string& quality_pref,
                             std::vector<Diag>* diag) {
    const json* st = jget(src_, "stream");
    if (!st) throw SourceError("source does not implement stream");
    const json* attempts = jget(*st, "attempts");
    if (!attempts || !attempts->is_array()) throw SourceError("stream.attempts missing");

    std::vector<json> order;
    for (const auto& a : *attempts) order.push_back(a);
    if (!quality_pref.empty()) {
        std::vector<json> head, tail;
        for (const auto& a : order) {
            std::string q = jstr(*jget(a, "quality"));
            (q == quality_pref ? head : tail).push_back(a);
        }
        order.clear();
        for (auto& a : head) order.push_back(a);
        for (auto& a : tail) order.push_back(a);
    }

    const std::vector<std::string> markers = encrypted_markers();

    std::vector<std::string> empty_vals = {"", ""};
    bool has_empty_list = false;
    std::vector<json> empty_list;
    if (const json* ev = jget(*st, "empty_values")) {
        if (ev->is_array()) {
            has_empty_list = true;
            for (const auto& x : *ev) empty_list.push_back(x);
        }
    }
    (void)empty_vals;

    const json track = track_to_json(t);

    for (const auto& at : order) {
        const std::string quality = jstr(*jget(at, "quality"));
        Ctx ctx;
        ctx.src = &src_;
        ctx.cookies = &cookies_;
        ctx.track = track;
        ctx.args["quality"] = quality;

        json resp;
        try {
            resp = do_request(src_, ctx, at.at("request"), opts);
        } catch (const std::exception& e) {
            if (diag) diag->push_back({quality, std::string("request failed: ") + e.what()});
            continue;
        }
        ctx.resp = resp;
        last_resp_ = resp;

        // 接口级错误码
        if (const json* err = jget(at, "error")) {
            if (const json* p = jget(*err, "path")) {
                json code = eval(*p, ctx);
                if (const json* okv = jget(*err, "ok_values")) {
                    if (okv->is_array()) {
                        bool pass = false;
                        for (const auto& x : *okv)
                            if (x == code) pass = true;
                        if (!pass) {
                            std::string hint = "see API response";
                            if (const json* hh = jget(*err, "hint")) hint = jstr(*hh);
                            if (diag)
                                diag->push_back({quality, "API returned code=" + jstr(code) + " (" + hint + ")"});
                            continue;
                        }
                    }
                }
            }
        }

        json urlv;
        if (const json* up = jget(at, "url")) urlv = eval(*up, ctx);
        std::string url = (urlv.is_null() || urlv.is_boolean()) ? "" : jstr(urlv);

        bool is_empty = url.empty();
        if (has_empty_list)
            for (const auto& x : empty_list)
                if (x == urlv) is_empty = true;
        if (is_empty) {
            if (diag) diag->push_back({quality, "purl empty (no entitlement for this quality / risk control / wrong field path)"});
            continue;
        }

        // 相对地址拼 CDN 主机头
        if (const json* bp = jget(at, "url_base")) {
            if (url.rfind("http", 0) != 0) {
                std::string base = jstr(eval(*bp, ctx));
                if (!base.empty()) {
                    if (base.back() != '/') base += "/";
                    while (!url.empty() && url[0] == '/') url.erase(url.begin());
                    url = base + url;
                }
            }
        }

        const std::string low = to_lower(url);
        for (const auto& m : markers) {
            if (low.find(m) != std::string::npos) {
                if (diag) diag->push_back({quality, "encrypted marker hit: " + m + ", skipped by policy"});
                StreamResult r;
                r.ok = false;
                r.quality = quality;
                r.error = "only encrypted stream available; not played by policy (no decryption)";
                return r;
            }
        }

        StreamResult r;
        r.ok = true;
        r.url = url;
        r.quality = quality;
        if (const json* f = jget(at, "format")) r.format = jstr(*f);
        if (const json* ex = jget(at, "expires_in")) {
            long long sec = jint(*ex);
            if (sec > 0) r.expires_at = now_ms() + sec * 1000;
        }
        return r;
    }

    StreamResult r;
    r.ok = false;
    r.error = "no playable stream for current account";
    return r;
}

std::string Engine::lyric(const Track& t) {
    const json* spec = jget(src_, "lyric");
    if (!spec) throw SourceError("source does not implement lyric");

    Ctx ctx;
    ctx.src = &src_;
    ctx.cookies = &cookies_;
    ctx.track = track_to_json(t);

    ctx.resp = do_request(src_, ctx, spec->at("request"), opts);
    last_resp_ = ctx.resp;

    json textv = ctx.resp;
    if (const json* tp = jget(*spec, "text")) textv = eval(*tp, ctx);
    if (textv.is_null()) return "";
    std::string text = jstr(textv);

    std::string enc = "plain";
    if (const json* e = jget(*spec, "encoding")) enc = to_lower(jstr(*e));

    if (enc == "base64") return b64_decode(text);
    if (enc == "html") return html_unescape(text);
    if (enc == "json_escaped") {
        try {
            json j = json::parse("\"" + text + "\"");
            return jstr(j);
        } catch (...) {
            return text;
        }
    }
    return text;
}

json Engine::raw_call(const std::string& op, const std::string& keyword, const Track& t) {
    Ctx ctx;
    ctx.src = &src_;
    ctx.cookies = &cookies_;

    if (op == "search") {
        const json* spec = jget(src_, "search");
        if (!spec) throw SourceError("source does not implement search");
        ctx.args["keyword"] = keyword;
        ctx.args["page"] = 1;
        ctx.resp = do_request(src_, ctx, spec->at("request"), opts);
    } else if (op == "stream") {
        const json* st = jget(src_, "stream");
        if (!st) throw SourceError("source does not implement stream");
        const json* at = jget(*st, "attempts");
        if (!at || !at->is_array() || at->empty()) throw SourceError("stream.attempts missing");
        ctx.track = track_to_json(t);
        ctx.args["quality"] = jstr(*jget((*at)[0], "quality"));
        ctx.resp = do_request(src_, ctx, (*at)[0].at("request"), opts);
    } else {
        const json* spec = jget(src_, "lyric");
        if (!spec) throw SourceError("source does not implement lyric");
        ctx.track = track_to_json(t);
        ctx.resp = do_request(src_, ctx, spec->at("request"), opts);
    }
    last_resp_ = ctx.resp;
    return last_resp_;
}

}  // namespace se
