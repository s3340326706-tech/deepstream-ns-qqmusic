// mp4_demux.cpp —— 最小可用的 MP4 音频解复用
//
// 只走一条路：找到 moov → 取第一个 hdlr 是 'soun' 的 trak → 从 stbl 里
// 拿 stsd(esds/ASC) 和 stts/stsc/stsz/stco 拼出样本表。够解 AAC 就行。
//
// 所有读取都做越界检查：这个函数会在「数据只下载了一部分」时被反复调用，
// 任何一处不检查就是直接读越界崩溃。数据不够时统一返回 NeedMore。

#include "player/mp4_demux.hpp"

#include <cstring>

namespace hd {
namespace {

struct Reader {
    const uint8_t* d = nullptr;
    size_t n         = 0;

    bool has(uint64_t off, uint64_t len) const
    {
        return off <= (uint64_t)n && len <= (uint64_t)n - off;
    }
    uint8_t u8(uint64_t off) const { return d[off]; }
    uint16_t u16(uint64_t off) const
    {
        return (uint16_t)(((uint16_t)d[off] << 8) | d[off + 1]);
    }
    uint32_t u32(uint64_t off) const
    {
        return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
               ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
    }
    uint64_t u64(uint64_t off) const
    {
        return ((uint64_t)u32(off) << 32) | (uint64_t)u32(off + 4);
    }
};

struct Box {
    uint32_t type     = 0;
    uint64_t off      = 0;   // box 起点（含 header）
    uint64_t size     = 0;   // 整个 box 的长度（含 header）
    uint64_t body_off = 0;
    uint64_t body_size = 0;
};

// 读一个 box 头。返回 false 表示数据不够（NeedMore）。
// end 是这个 box 所在父容器/文件的结束位置，用于校验 size 合法性。
bool read_box(const Reader& r, uint64_t off, uint64_t end, Box* b)
{
    if (!r.has(off, 8)) return false;
    uint64_t size = r.u32(off);
    b->type       = r.u32(off + 4);
    b->off        = off;

    if (size == 1) {
        if (!r.has(off + 8, 8)) return false;
        size         = r.u64(off + 8);
        b->body_off  = off + 16;
    } else if (size == 0) {
        // size 0 表示「一直到文件尾」
        if (end <= off) return false;
        size        = end - off;
        b->body_off = off + 8;
    } else {
        b->body_off = off + 8;
    }

    if (size < (b->body_off - off)) return false;   // 明显损坏
    b->size      = size;
    b->body_size = size - (b->body_off - off);
    return true;
}

// 列出 [off, end) 范围内的直接子 box。数据不足时返回 false。
bool list_children(const Reader& r, uint64_t off, uint64_t end, std::vector<Box>* out)
{
    while (off + 8 <= end) {
        Box b;
        if (!read_box(r, off, end, &b)) return false;
        if (b.off + b.size > end) return false;   // 子 box 越界 → 数据还没到
        out->push_back(b);
        if (b.size == 0) break;
        off = b.off + b.size;
    }
    return true;
}

const Box* find(const std::vector<Box>& v, uint32_t type)
{
    for (const auto& b : v)
        if (b.type == type) return &b;
    return nullptr;
}

// —— esds 里的描述符（ISO 14496-1）——
// tag 0x03 ES_Descriptor → 0x04 DecoderConfig → 0x05 DecoderSpecificInfo(=ASC)
uint32_t desc_len(const uint8_t* p, size_t n, size_t* used)
{
    uint32_t len = 0;
    size_t i     = 0;
    while (i < 4 && i < n) {
        uint8_t c = p[i++];
        len       = (len << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    *used = i;
    return len;
}

bool walk_desc(const uint8_t* p, size_t n, std::vector<uint8_t>* asc, int depth)
{
    if (depth > 8) return false;
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t tag = p[i++];
        size_t used = 0;
        uint32_t len = desc_len(p + i, n - i, &used);
        i += used;
        if (i > n || (size_t)len > n - i) return false;

        const uint8_t* body = p + i;
        if (tag == 0x03) {   // ES_Descriptor
            size_t skip = 3;
            if (len >= 3) {
                uint8_t flags = body[2];
                if (flags & 0x80) skip += 2;
                if (flags & 0x40) {
                    if (len < skip + 1) return false;
                    skip += 1 + body[skip];
                }
                if (flags & 0x20) skip += 2;
                if (skip <= len && walk_desc(body + skip, len - skip, asc, depth + 1)) return true;
            }
        } else if (tag == 0x04) {   // DecoderConfigDescriptor
            if (len >= 13 && walk_desc(body + 13, len - 13, asc, depth + 1)) return true;
        } else if (tag == 0x05) {   // DecoderSpecificInfo
            asc->assign(body, body + len);
            return true;
        }
        i += len;
    }
    return false;
}

// 在 mp4a/enca 入口里找 esds（可能在 wave 里，递归找）
const Box* find_esds(const Reader& r, const std::vector<Box>& kids, int depth)
{
    for (const auto& b : kids) {
        if (b.type == 0x65736473) return &b;   // 'esds'
        if (b.type == 0x77617665 && depth < 3) {   // 'wave'
            std::vector<Box> sub;
            if (list_children(r, b.body_off, b.off + b.size, &sub)) {
                if (const Box* f = find_esds(r, sub, depth + 1)) return f;
            }
        }
    }
    return nullptr;
}

bool parse_moov(const Reader& r, const Box& moov, Mp4Audio* out, std::string* error)
{
    std::vector<Box> top;
    if (!list_children(r, moov.body_off, moov.off + moov.size, &top)) return false;

    for (const auto& trak : top) {
        if (trak.type != 0x7472616b) continue;   // 'trak'

        std::vector<Box> tkids;
        if (!list_children(r, trak.body_off, trak.off + trak.size, &tkids)) return false;
        const Box* mdia = find(tkids, 0x6d646961);   // 'mdia'
        if (!mdia) continue;

        std::vector<Box> mkids;
        if (!list_children(r, mdia->body_off, mdia->off + mdia->size, &mkids)) return false;

        // 只要音频轨
        const Box* hdlr = find(mkids, 0x68646c72);   // 'hdlr'
        const Box* mdhd = find(mkids, 0x6d646864);   // 'mdhd'
        const Box* minf = find(mkids, 0x6d696e66);   // 'minf'
        if (!hdlr || !mdhd || !minf) continue;
        if (hdlr->body_size < 12) continue;
        if (r.u32(hdlr->body_off + 8) != 0x736f756e) continue;   // 'soun'

        // —— mdhd：timescale / duration ——
        {
            uint64_t o = mdhd->body_off;
            if (mdhd->body_size < 20) continue;
            uint8_t ver = r.u8(o);
            if (ver == 1) {
                if (mdhd->body_size < 34) continue;
                out->timescale   = r.u32(o + 20);
                out->duration_ts = r.u64(o + 24);
            } else {
                out->timescale   = r.u32(o + 12);
                out->duration_ts = r.u32(o + 16);
            }
        }

        std::vector<Box> fkids;
        if (!list_children(r, minf->body_off, minf->off + minf->size, &fkids)) return false;
        const Box* stbl = find(fkids, 0x7374626c);   // 'stbl'
        if (!stbl) continue;

        std::vector<Box> skids;
        if (!list_children(r, stbl->body_off, stbl->off + stbl->size, &skids)) return false;

        const Box* stsd = find(skids, 0x73747364);   // 'stsd'
        const Box* stts = find(skids, 0x73747473);   // 'stts'
        const Box* stsc = find(skids, 0x73747363);   // 'stsc'
        const Box* stsz = find(skids, 0x7374737a);   // 'stsz'
        const Box* stco = find(skids, 0x7374636f);   // 'stco'
        const Box* co64 = find(skids, 0x636f3634);   // 'co64'
        if (!stsd || !stsz || !stsc || (!stco && !co64)) {
            *error = "mp4: audio track missing stbl tables";
            return false;
        }

        // —— stsd：拿 ASC ——
        {
            uint64_t o = stsd->body_off;
            if (stsd->body_size < 16) {
                *error = "mp4: stsd too small";
                return false;
            }
            uint32_t count = r.u32(o + 4);
            if (count == 0) {
                *error = "mp4: stsd empty";
                return false;
            }
            Box entry;
            if (!read_box(r, o + 8, stsd->off + stsd->size, &entry)) {
                *error = "mp4: stsd entry truncated";
                return false;
            }
            if (entry.type != 0x6d703461) {   // 'mp4a'
                *error = "mp4: not an AAC track (sample entry is not mp4a)";
                return false;
            }
            // mp4a 的固定字段 28 字节：reserved(6)+dref(2)+ver/rev/vendor(8)
            //                            +ch(2)+bits(2)+pre(2)+rsv(2)+rate(4)
            if (entry.body_size >= 28) {
                out->channels    = r.u16(entry.body_off + 16);
                out->sample_rate = r.u32(entry.body_off + 24) >> 16;
            }
            std::vector<Box> ekids;
            if (list_children(r, entry.body_off + 28, entry.off + entry.size, &ekids)) {
                if (const Box* esds = find_esds(r, ekids, 0)) {
                    std::vector<uint8_t> asc;
                    // esds body: version+flags(4) 之后是描述符
                    if (esds->body_size > 4) {
                        const uint8_t* p = r.d + esds->body_off + 4;
                        size_t len       = (size_t)esds->body_size - 4;
                        if (walk_desc(p, len, &asc, 0)) out->asc = asc;
                    }
                }
            }
            if (out->asc.empty()) {
                *error = "mp4: no AudioSpecificConfig (esds)";
                return false;
            }
        }

        // —— stsz：每个样本的字节数 ——
        std::vector<uint32_t> sizes;
        {
            uint64_t o            = stsz->body_off;
            uint32_t fixed        = r.u32(o + 4);
            uint32_t count        = r.u32(o + 8);
            if (count > 10000000u) {
                *error = "mp4: absurd sample count";
                return false;
            }
            if (fixed != 0) {
                sizes.assign(count, fixed);
            } else {
                if (stsz->body_size < 12 + (uint64_t)count * 4) {
                    *error = "mp4: stsz truncated";
                    return false;
                }
                sizes.resize(count);
                for (uint32_t i = 0; i < count; ++i) sizes[i] = r.u32(o + 12 + (uint64_t)i * 4);
            }
            if (sizes.empty()) {
                *error = "mp4: no samples";
                return false;
            }
        }

        // —— stsc：chunk 里有多少样本 ——
        struct StscEntry {
            uint32_t first_chunk;
            uint32_t per_chunk;
        };
        std::vector<StscEntry> stscs;
        {
            uint64_t o     = stsc->body_off;
            uint32_t count = r.u32(o + 4);
            if (count == 0 || stsc->body_size < 8 + (uint64_t)count * 12) {
                *error = "mp4: stsc truncated";
                return false;
            }
            stscs.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                stscs[i].first_chunk = r.u32(o + 8 + (uint64_t)i * 12);
                stscs[i].per_chunk   = r.u32(o + 8 + (uint64_t)i * 12 + 4);
            }
        }

        // —— stco / co64：每个 chunk 的偏移 ——
        std::vector<uint64_t> chunks;
        {
            const Box* c           = stco ? stco : co64;
            const bool wide        = (stco == nullptr);
            uint64_t o             = c->body_off;
            uint32_t count         = r.u32(o + 4);
            const uint64_t step    = wide ? 8 : 4;
            if (count == 0 || c->body_size < 8 + (uint64_t)count * step) {
                *error = "mp4: chunk offsets truncated";
                return false;
            }
            chunks.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                chunks[i] = wide ? r.u64(o + 8 + (uint64_t)i * 8) : (uint64_t)r.u32(o + 8 + (uint64_t)i * 4);
        }

        // —— stts：每样本时长（展开）——
        if (stts && stts->body_size >= 8) {
            uint64_t o     = stts->body_off;
            uint32_t count = r.u32(o + 4);
            if (count <= 4096 && stts->body_size >= 8 + (uint64_t)count * 8) {
                out->durations.reserve(sizes.size());
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t n     = r.u32(o + 8 + (uint64_t)i * 8);
                    uint32_t delta = r.u32(o + 8 + (uint64_t)i * 8 + 4);
                    while (n-- > 0 && out->durations.size() < sizes.size())
                        out->durations.push_back(delta);
                }
            }
        }

        // —— 拼样本表 ——
        out->samples.reserve(sizes.size());
        size_t cursor  = 0;
        size_t next_stsc = 0;
        for (size_t ci = 0; ci < chunks.size() && cursor < sizes.size(); ++ci) {
            const uint32_t chunk_no = (uint32_t)(ci + 1);
            while (next_stsc + 1 < stscs.size() && stscs[next_stsc + 1].first_chunk <= chunk_no)
                ++next_stsc;
            const uint32_t per = stscs.empty() ? 0 : stscs[next_stsc].per_chunk;
            if (per == 0) break;

            uint64_t off = chunks[ci];
            for (uint32_t k = 0; k < per && cursor < sizes.size(); ++k) {
                Mp4Sample s;
                s.offset = off;
                s.size   = sizes[cursor];
                out->samples.push_back(s);
                off += sizes[cursor];
                ++cursor;
            }
        }
        if (out->samples.empty()) {
            *error = "mp4: empty sample table";
            return false;
        }

        // —— 起始时间表（秒）——
        out->starts.resize(out->samples.size());
        {
            double acc = 0.0;
            const double ts = out->timescale ? 1.0 / (double)out->timescale : 0.0;
            for (size_t i = 0; i < out->samples.size(); ++i) {
                out->starts[i] = acc;
                uint32_t d     = (i < out->durations.size() && out->durations[i])
                                     ? out->durations[i]
                                     : out->frame_len;
                acc += (double)d * ts;
            }
        }
        if (out->duration_ts == 0 && !out->durations.empty()) {
            uint64_t sum = 0;
            for (uint32_t d : out->durations) sum += d;
            out->duration_ts = sum;
        }
        return true;
    }

    *error = "mp4: no audio track";
    return false;
}

}  // namespace

size_t Mp4Audio::sample_at(double sec) const
{
    if (starts.empty()) return 0;
    if (sec <= 0.0) return 0;
    size_t lo = 0, hi = starts.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (starts[mid] <= sec) lo = mid;
        else hi = mid;
    }
    return lo;
}

Mp4Parse mp4_parse_audio(const uint8_t* data, size_t size, Mp4Audio* out, std::string* error)
{
    Reader r{data, size};

    // 快速失败的判断：m4a 的第二个 box 一般是 ftyp
    if (size < 12) return Mp4Parse::NeedMore;

    uint64_t off      = 0;
    bool saw_mdat_end = false;
    const uint64_t file_end = (uint64_t)size;   // 已下载的部分

    while (off + 8 <= size) {
        Box b;
        if (!read_box(r, off, file_end, &b)) return Mp4Parse::NeedMore;

        if (b.type == 0x6d6f6f76) {   // 'moov'
            if (b.off + b.size > size) return Mp4Parse::NeedMore;   // moov 还没下完
            std::string err;
            Mp4Audio tmp;
            if (!parse_moov(r, b, &tmp, &err)) {
                if (error) *error = err.empty() ? "mp4: parse moov failed" : err;
                return Mp4Parse::Error;
            }
            if (tmp.sample_rate == 0) tmp.sample_rate = 44100;
            if (tmp.channels == 0) tmp.channels = 2;
            if (tmp.timescale == 0) tmp.timescale = tmp.sample_rate;
            *out = std::move(tmp);
            return Mp4Parse::Ok;
        }

        if (b.type == 0x6d646174) saw_mdat_end = true;   // 'mdat'

        if (b.size == 0) return Mp4Parse::NeedMore;      // 到文件尾，moov 还没出现
        if (b.off + b.size > size) {
            // 这个 box 还没下完。如果它是 mdat，moov 在它后面 → 必须等下载完。
            // 否则（比如 ftyp 就超大，不正常）也等。
            return Mp4Parse::NeedMore;
        }
        off = b.off + b.size;
    }

    (void)saw_mdat_end;
    return Mp4Parse::NeedMore;   // 遍历到已下载末尾都没见到 moov
}

void mp4_build_adts_header(const uint8_t* asc, size_t asc_len, size_t payload, uint8_t out[7])
{
    unsigned obj  = 2;   // AAC-LC
    unsigned freq = 4;   // 44100
    unsigned ch   = 2;
    if (asc && asc_len >= 2) {
        obj  = (asc[0] >> 3) & 0x1F;
        freq = ((unsigned)(asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01);
        ch   = (asc[1] >> 3) & 0x0F;
        if (ch == 0) ch = 2;   // 0 表示「由 PCE 决定」，这里当立体声
    }
    // ADTS 的 profile 只有 2 位，能表示 Main/LC/SSR/LTP；
    // HE-AAC（object type 5/29）的核心仍是 LC，一律按 LC 写。
    unsigned profile = (obj >= 1 && obj <= 4) ? (obj - 1) : 1;
    if (profile > 3) profile = 1;

    const unsigned len = (unsigned)(payload + 7);
    out[0] = 0xFF;
    out[1] = 0xF1;   // MPEG-4, layer 0, protection absent
    out[2] = (uint8_t)((profile << 6) | ((freq & 0x0F) << 2) | ((ch >> 2) & 0x01));
    out[3] = (uint8_t)(((ch & 0x03) << 6) | ((len >> 11) & 0x03));
    out[4] = (uint8_t)((len >> 3) & 0xFF);
    out[5] = (uint8_t)(((len & 0x07) << 5) | 0x1F);
    out[6] = 0xFC;
}

}  // namespace hd
