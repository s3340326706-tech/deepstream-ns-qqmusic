// aac_selftest.cpp —— PC 侧验证 m4a(AAC) 解码链路
//
// 为什么要单独写这个：Switch 上崩溃/无声，日志只能告诉我"失败"，
// 说不出"到底哪一步错了"。这个程序在 PC 上把同一条链路跑一遍：
//   读文件 → mp4_demux 解容器 → faad2 解码 → 写 wav
// 任何一步错都能立刻看到（帧数、错误码、时长对不对）。
//
// 编译见 tools/build_aac_selftest.sh
// 用法：aac_selftest <file.m4a> [out.wav]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "neaacdec.h"
#include "player/mp4_demux.hpp"

static std::vector<uint8_t> slurp(const char* path)
{
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize((size_t)n);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

static void write_wav(const char* path, const std::vector<int16_t>& pcm, unsigned rate,
                      unsigned ch)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;

    const uint32_t data_bytes = (uint32_t)(pcm.size() * 2);
    const uint32_t byte_rate  = rate * ch * 2;
    const uint16_t block_align = (uint16_t)(ch * 2);
    uint32_t riff  = 36 + data_bytes;
    uint32_t fmt   = 16;
    uint16_t pcmf  = 1;
    uint32_t sr    = rate;
    uint16_t bits  = 16;
    uint32_t dlen  = data_bytes;
    uint16_t chs   = (uint16_t)ch;
    uint32_t br    = byte_rate;
    uint16_t ba    = block_align;

    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f);
    std::fwrite(&pcmf, 2, 1, f);
    std::fwrite(&chs, 2, 1, f);
    std::fwrite(&sr, 4, 1, f);
    std::fwrite(&br, 4, 1, f);
    std::fwrite(&ba, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dlen, 4, 1, f);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: aac_selftest <file.m4a> [out.wav]\n");
        return 2;
    }

    std::vector<uint8_t> d = slurp(argv[1]);
    if (d.empty()) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    std::printf("file: %s  %zu bytes\n", argv[1], d.size());

    // —— 1. 半截数据：模拟"边下边播时刚拿到一半就开始探测" ——
    // 期望 NeedMore（moov 在尾部），不期望 Error。
    {
        hd::Mp4Audio tmp;
        std::string err;
        auto st = hd::mp4_parse_audio(d.data(), d.size() / 2, &tmp, &err);
        const char* name = st == hd::Mp4Parse::Ok        ? "Ok"
                           : st == hd::Mp4Parse::NeedMore ? "NeedMore"
                                                          : "Error";
        std::printf("[1] half parse -> %s %s\n", name, err.empty() ? "" : err.c_str());
        if (st == hd::Mp4Parse::Error) return 1;
    }

    // —— 2. 完整解析 ——
    hd::Mp4Audio a;
    std::string err;
    auto st = hd::mp4_parse_audio(d.data(), d.size(), &a, &err);
    if (st != hd::Mp4Parse::Ok) {
        std::printf("[2] FAILED state=%d %s\n", (int)st, err.c_str());
        return 1;
    }
    std::printf("[2] ok  rate=%u ch=%u timescale=%u dur=%.3fs samples=%zu asc=", a.sample_rate,
                a.channels, a.timescale, a.duration_sec(), a.samples.size());
    for (uint8_t b : a.asc) std::printf("%02X ", b);
    std::printf("\n");

    // 样本偏移必须在文件内
    for (size_t i = 0; i < a.samples.size(); ++i) {
        if (a.samples[i].offset + a.samples[i].size > d.size()) {
            std::printf("[2] BAD sample %zu: off=%llu size=%u > file %zu\n", i,
                        (unsigned long long)a.samples[i].offset, a.samples[i].size, d.size());
            return 1;
        }
    }

    // —— 3. 初始化解码器 ——
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = FAAD_FMT_16BIT;
    cfg->downMatrix   = 1;
    NeAACDecSetConfiguration(h, cfg);

    unsigned long sr  = 0;
    unsigned char ch  = 0;
    if (NeAACDecInit2(h, a.asc.data(), (unsigned long)a.asc.size(), &sr, &ch) != 0) {
        std::printf("[3] FAILED NeAACDecInit2\n");
        return 1;
    }
    std::printf("[3] faad2 init2 ok  %lu Hz %u ch\n", sr, (unsigned)ch);

    // —— 4. 逐样本解码 ——
    std::vector<uint8_t> fed;
    std::vector<int16_t> pcm;
    size_t ok_frames = 0, total_frames = 0;
    int errors = 0;
    unsigned out_rate = sr;
    unsigned out_ch   = ch;

    for (size_t i = 0; i < a.samples.size(); ++i) {
        const hd::Mp4Sample& s = a.samples[i];
        fed.resize(7 + (size_t)s.size);
        hd::mp4_build_adts_header(a.asc.data(), a.asc.size(), s.size, fed.data());
        std::memcpy(fed.data() + 7, d.data() + s.offset, s.size);

        NeAACDecFrameInfo fi;
        std::memset(&fi, 0, sizeof(fi));
        void* out = NeAACDecDecode(h, &fi, fed.data(), (unsigned long)fed.size());
        if (!out || fi.error || fi.channels == 0 || fi.samples == 0) {
            if (errors < 5)
                std::printf("    frame %zu error=%u (%s)\n", i, fi.error,
                            NeAACDecGetErrorMessage(fi.error));
            ++errors;
            continue;
        }
        ++ok_frames;
        total_frames += fi.samples / fi.channels;
        if (fi.samplerate) out_rate = fi.samplerate;
        out_ch = fi.channels;
        const int16_t* p = (const int16_t*)out;
        pcm.insert(pcm.end(), p, p + fi.samples);
    }

    NeAACDecClose(h);

    std::printf("[4] decoded %zu/%zu frames, %zu pcm frames (%.3fs @ %uHz/%uch), errors=%d\n",
                ok_frames, a.samples.size(), total_frames,
                out_rate ? (double)total_frames / (double)out_rate : 0.0, out_rate, out_ch,
                errors);

    if (total_frames == 0) {
        std::printf("[4] FAILED: 一帧都没解出来\n");
        return 1;
    }

    // 时长误差检查：解码出的秒数应接近容器声称的时长（AAC 有 priming 帧，
    // 差个几十毫秒正常；差一倍说明时长算法错了）
    const double got = out_rate ? (double)total_frames / (double)out_rate : 0.0;
    const double want = a.duration_sec();
    if (want > 0.5) {
        const double diff = got - want;
        std::printf("[5] duration: decoded %.3fs vs container %.3fs (diff %+.3fs)\n", got, want,
                    diff);
        if (diff < -0.15 || diff > 0.35) {
            std::printf("[5] WARN: 时长偏差过大\n");
        }
    }

    if (argc >= 3) {
        write_wav(argv[2], pcm, out_rate, out_ch);
        std::printf("[6] wrote %s (%zu samples)\n", argv[2], pcm.size());
    }

    std::printf("[ok] AAC 链路通过\n");
    return 0;
}
