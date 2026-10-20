#include "se/util.hpp"

#include <cmath>
#include <cstring>

namespace se {

// --------------------------------------------------------------------------
// MD5
// --------------------------------------------------------------------------
namespace {

inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5_block(uint32_t st[4], const uint8_t* p) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);

    // K[i] = floor(2^32 * |sin(i+1)|)，用公式生成，避免手抄 64 个数出错
    static uint32_t K[64];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 64; ++i) {
            double v = std::fabs(std::sin((double)(i + 1))) * 4294967296.0;
            K[i] = (uint32_t)std::floor(v);
        }
        ready = true;
    }

    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + m[g], S[i]);
        a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

// SHA-1 一块
void sha1_block(uint32_t st[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

std::string hex32(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(H[d[i] >> 4]);
        out.push_back(H[d[i] & 0xF]);
    }
    return out;
}

std::vector<uint8_t> pad(const std::string& in, bool big_endian_len) {
    std::vector<uint8_t> buf(in.begin(), in.end());
    uint64_t bits = (uint64_t)in.size() * 8ull;
    buf.push_back(0x80);
    while (buf.size() % 64 != 56) buf.push_back(0x00);
    for (int i = 0; i < 8; ++i) {
        uint8_t byte = (uint8_t)((bits >> (8 * (big_endian_len ? (7 - i) : i))) & 0xFF);
        buf.push_back(byte);
    }
    return buf;
}

}  // namespace

std::string md5_hex(const std::string& in) {
    uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::vector<uint8_t> buf = pad(in, false);
    for (size_t off = 0; off < buf.size(); off += 64) md5_block(st, buf.data() + off);
    uint8_t out[16];
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = (uint8_t)(st[i] & 0xFF);
        out[i * 4 + 1] = (uint8_t)((st[i] >> 8) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((st[i] >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((st[i] >> 24) & 0xFF);
    }
    return hex32(out, 16);
}

std::string sha1_hex(const std::string& in) {
    uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> buf = pad(in, true);
    for (size_t off = 0; off < buf.size(); off += 64) sha1_block(st, buf.data() + off);
    uint8_t out[20];
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = (uint8_t)((st[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (uint8_t)((st[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((st[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (uint8_t)(st[i] & 0xFF);
    }
    return hex32(out, 20);
}

std::string base64_encode(const std::string& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(T[(v >> 6) & 63]);
        out.push_back(T[v & 63]);
        i += 3;
    }
    size_t rest = in.size() - i;
    if (rest == 1) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        uint32_t v = ((uint32_t)(uint8_t)in[i] << 16) | ((uint32_t)(uint8_t)in[i + 1] << 8);
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(T[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string url_encode(const std::string& in) {
    static const char* H = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(H[c >> 4]);
            out.push_back(H[c & 0xF]);
        }
    }
    return out;
}

std::string digits_only(const std::string& in) {
    std::string out;
    for (char c : in)
        if (c >= '0' && c <= '9') out.push_back(c);
    return out;
}

uint32_t hash33(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h += (h << 5) + (uint32_t)c;
        h &= 0xFFFFFFFFu;
    }
    return h & 0x7FFFFFFFu;
}

uint32_t gtk_of(const std::string& skey) {
    uint32_t h = 5381u;
    for (unsigned char c : skey) {
        h += (h << 5) + (uint32_t)c;
        h &= 0xFFFFFFFFu;
    }
    return h & 0x7FFFFFFFu;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

}  // namespace se
