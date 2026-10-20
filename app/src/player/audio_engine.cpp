// audio_engine.cpp —— 边下边播：下载线程 + 流式解码 + 重采样 + audout
//
// 数据流：
//   [curl 下载线程] 追加字节到 buf_（只增不减，seek 回退要能重读）
//        ↓  read 回调阻塞等数据（"播到哪下到哪"的关键）
//   [解码线程] libFLAC / mpg123 / faad2 → float → 线性重采样 48k → 环形缓冲
//        ↓
//   [pump 线程] 环形缓冲 → audout 的 4 个 AudioOutBuffer 循环灌
//
// 为什么换掉 dr_flac：dr_flac 只接受"完整内存缓冲"，做不到边下边播。
// libFLAC 的 init_stream 提供 read/seek/tell/length/eof 回调，天生支持流。
//
// AAC 为什么比 MP3/FLAC 麻烦：
//   QQ 音乐的 m4a 是 MP4 容器，音频数据被包在 mdat 里，moov 里有样本表。
//   faad2 只认 AAC 帧，所以先经 mp4_demux 拆出 ASC + 每帧偏移，再逐帧补
//   ADTS 头喂进去。另外 moov 常写在文件尾（不是 faststart），流式拿不到，
//   这类文件必须等整首下完才能播 —— m4a 一般 3~10MB，可以接受。

#include "player/audio_engine.hpp"

#include <algorithm>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include <curl/curl.h>

#include <FLAC/stream_decoder.h>
#include <mpg123.h>
#include <neaacdec.h>

#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>  // memalign：audout 要求缓冲 0x1000 对齐
#define CA_PATH "romfs:/cacert.pem"
#endif

namespace hd {
namespace {

constexpr size_t kMaxAudioBytes = 128ull * 1024 * 1024;  // 缓冲上限
constexpr size_t kStartBytes = 64 * 1024;                // 攒够这么多就能判断格式
constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;

// —— 抗抖动的三个数，全是实测调出来的 ——
// kBufFrames × kOutBufCount 是硬件侧能顶住的断供时长：2048 帧 = 42.7ms，6 块 ≈ 256ms。
// 原来 1024 × 4 只有 85ms，Switch 的调度抖一下就见底，听感就是"卡一下"。
constexpr int kBufFrames = 2048;
constexpr int kOutBufCount = 6;
// 解码侧环形缓冲：2 秒。下载抖动、解码器偶尔慢半拍全靠它吸收。
constexpr int kRingFrames = 96000;

// —— 预缓冲 ——
// 边下边播 ≠ 边下边听。开播前必须先攒够这么多秒的**源数据**，
// 否则网络只要一次 RTT 抖动（QQ 音乐 CDN 常有 200~500ms 的空档），
// 缓冲瞬间吃穿，用户听到的就是卡顿。3.5 秒是"起播不算慢 + 够抗一次抖动"的折中。
constexpr double kPrebufferSecs = 3.5;
constexpr size_t kPrebufferMin = 192 * 1024;      // 再怎么也要 192KB（约 1 秒无损）
constexpr size_t kPrebufferMax = 2 * 1024 * 1024;  // 上限，别让起播等太久
constexpr int kPrebufferWaitMs = 8000;             // 最多等 8 秒，网络实在慢也放行

struct DlSink {
    std::vector<uint8_t>* buf = nullptr;
    std::mutex* mu = nullptr;
    std::condition_variable* cv = nullptr;
    std::atomic<bool>* abort = nullptr;
    bool* failed = nullptr;
    size_t* total = nullptr;
    bool overflow = false;
};

size_t dl_write_cb(char* p, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<DlSink*>(userdata);
    size_t n = size * nmemb;
    {
        std::lock_guard<std::mutex> lk(*s->mu);
        if (s->buf->size() + n > kMaxAudioBytes) {
            s->overflow = true;
            return 0;
        }
        s->buf->insert(s->buf->end(), (uint8_t*)p, (uint8_t*)p + n);
    }
    s->cv->notify_all();
    return n;
}

int dl_progress_cb(void* userdata, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t) {
    auto* s = static_cast<DlSink*>(userdata);
    if (dltotal > 0) *s->total = (size_t)dltotal;
    return (*s->abort) ? 1 : 0;
}

const char* sniff_codec(const uint8_t* d, size_t n) {
    if (n >= 4 && std::memcmp(d, "fLaC", 4) == 0) return "FLAC";
    // MP4/M4A：第 4 字节起是 'ftyp'
    if (n >= 8 && std::memcmp(d + 4, "ftyp", 4) == 0) return "M4A";
    if (n >= 3 && std::memcmp(d, "ID3", 3) == 0) return "MP3";
    // ADTS 必须在 MP3 之前判：两者的 sync 都是 0xFFF 开头，
    // ADTS 的 layer 位恒为 00（d[1] & 0xF6 == 0xF0），MP3 不会。
    // 顺序反了会把 AAC 裸流当 MP3 交给 mpg123，结果是噪音或直接失败。
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return "AAC";
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "MP3";
    if (n >= 4 && std::memcmp(d, "OggS", 4) == 0) return "OGG";
    return "unknown";
}

// MP4 里的 AAC 是裸帧（没有 ADTS 头）。faad2 靠 ADTS 头里的
// frame_length 找帧边界，所以喂之前得补一个 7 字节的头。
// 实现放在 mp4_demux.cpp —— PC 自测程序和播放器共用同一份，
// 避免"测的是一套、跑的是另一套"。
void build_adts_header(const std::vector<uint8_t>& asc, size_t payload, uint8_t out[7])
{
    mp4_build_adts_header(asc.data(), asc.size(), payload, out);
}

// 从 ADTS 头里取整帧长度（含头）。返回 0 表示头部不完整或不是 ADTS。
size_t adts_frame_len(const uint8_t* p, size_t n)
{
    if (n < 7) return 0;
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    size_t len = ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    const size_t hdr = (p[1] & 0x01) ? 7 : 9;   // protection_absent=0 时有 2 字节 CRC
    if (len < hdr) return 0;
    return len;
}

// 直接按 FLAC 规范解析 STREAMINFO（34 字节）：
//   16b min_bs | 16b max_bs | 24b min_fs | 24b max_fs |
//   20b sample_rate | 3b channels-1 | 5b bps-1 | 36b total_samples | 128b md5
// 不用 FLAC__stream_decoder_get_sample_rate()：实测某些版本在
// SEARCH_FOR_FRAME_SYNC 状态下这几个 accessor 返回 0（total_samples 却正常）。
bool parse_streaminfo(const uint8_t* d, size_t n, unsigned* rate, unsigned* ch, uint64_t* total) {
    if (!d || n < 8) return false;
    size_t off = (std::memcmp(d, "fLaC", 4) == 0) ? 4 : 0;
    if (n < off + 4 + 18) return false;
    if ((d[off] & 0x7F) != 0) return false;   // 第一个 metadata 块必须是 STREAMINFO
    const uint8_t* p = d + off + 4;
    *rate = ((unsigned)p[10] << 12) | ((unsigned)p[11] << 4) | ((unsigned)p[12] >> 4);
    *ch = (((unsigned)p[12] >> 1) & 0x07) + 1;
    *total = ((uint64_t)(p[13] & 0x0F) << 32) | ((uint64_t)p[14] << 24) |
             ((uint64_t)p[15] << 16) | ((uint64_t)p[16] << 8) | (uint64_t)p[17];
    return *rate > 0 && *ch > 0;
}

inline int16_t to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)std::lrint(v * 32767.0f);
}

}  // namespace

// ─────────────────────────────────────────────────────────────
// 平台相关（环形缓冲 + 输出设备）
// ─────────────────────────────────────────────────────────────
struct AudioEngine::Impl {
    std::mutex mu;
    std::condition_variable cv_data;
    std::condition_variable cv_space;

    std::vector<int16_t> ring;
    size_t head = 0;
    size_t size = 0;
    bool eof = false;
    bool abort = false;
    bool paused = false;

    std::thread feeder;
    std::thread pump;
    bool out_ready = false;

#ifdef __SWITCH__
    AudioOutBuffer buf[kOutBufCount]{};
    int16_t* mem[kOutBufCount] = {nullptr};
#endif
};

namespace {

size_t ring_write(AudioEngine::Impl* im, const int16_t* src, size_t frames) {
    std::unique_lock<std::mutex> lk(im->mu);
    size_t done = 0;
    while (done < frames && !im->abort) {
        size_t space = (size_t)kRingFrames - im->size;
        if (space == 0) {
            im->cv_space.wait_for(lk, std::chrono::milliseconds(20));
            continue;
        }
        size_t n = std::min(space, frames - done);
        size_t tail = (im->head + im->size) % kRingFrames;
        size_t first = std::min(n, (size_t)kRingFrames - tail);
        std::memcpy(&im->ring[tail * kOutChannels], src + done * kOutChannels,
                    first * kOutChannels * sizeof(int16_t));
        if (n > first) {
            std::memcpy(&im->ring[0], src + (done + first) * kOutChannels,
                        (n - first) * kOutChannels * sizeof(int16_t));
        }
        im->size += n;
        done += n;
        im->cv_data.notify_all();
    }
    return done;
}

size_t ring_read(AudioEngine::Impl* im, int16_t* dst, size_t frames) {
    std::unique_lock<std::mutex> lk(im->mu);
    while (im->size == 0 && !im->eof && !im->abort) {
        im->cv_data.wait_for(lk, std::chrono::milliseconds(20));
    }
    size_t n = std::min(frames, im->size);
    if (n > 0) {
        size_t first = std::min(n, (size_t)kRingFrames - im->head);
        std::memcpy(dst, &im->ring[im->head * kOutChannels],
                    first * kOutChannels * sizeof(int16_t));
        if (n > first) {
            std::memcpy(dst + first * kOutChannels, &im->ring[0],
                        (n - first) * kOutChannels * sizeof(int16_t));
        }
        im->head = (im->head + n) % kRingFrames;
        im->size -= n;
        im->cv_space.notify_all();
    }
    return n;
}

// —— libFLAC 回调（C 函数指针，client_data 是 AudioEngine*）——
FLAC__StreamDecoderReadStatus flac_read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[],
                                          size_t* bytes, void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    bool eof = false;
    size_t n = e->impl_pull(buffer, *bytes, &eof);
    *bytes = n;
    if (n > 0) return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    return eof ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
               : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

FLAC__StreamDecoderSeekStatus flac_seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 offset,
                                          void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    if (!e->impl_seek_bytes(offset)) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus flac_tell_cb(const FLAC__StreamDecoder*, FLAC__uint64* offset,
                                          void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    *offset = e->impl_tell();
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus flac_length_cb(const FLAC__StreamDecoder*, FLAC__uint64* len,
                                              void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    *len = e->info().bytes;   // Content-Length，未知就 0
    return *len > 0 ? FLAC__STREAM_DECODER_LENGTH_STATUS_OK
                    : FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
}

FLAC__bool flac_eof_cb(const FLAC__StreamDecoder*, void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    bool eof = false;
    e->impl_pull(nullptr, 0, &eof);
    return eof;
}

FLAC__StreamDecoderWriteStatus flac_write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                            const FLAC__int32* const buffer[], void* client_data) {
    auto* e = static_cast<AudioEngine*>(client_data);
    e->impl_push_i32(buffer, frame->header.channels, frame->header.blocksize,
                     frame->header.bits_per_sample);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void flac_error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* cd) {
    auto* e = static_cast<AudioEngine*>(cd);
    e->impl_fail(std::string("flac decode error: ") + FLAC__StreamDecoderErrorStatusString[status]);
}

// STREAMINFO 直接从 metadata 回调里取，不依赖 get_sample_rate() 一类 accessor
// （某些 libFLAC 版本在 SEARCH_FOR_FRAME_SYNC 状态下这几个 accessor 返回 0）。
void flac_meta_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* md, void* cd) {
    if (!md || md->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    auto* e = static_cast<AudioEngine*>(cd);
    e->impl_set_streaminfo(md->data.stream_info.sample_rate, md->data.stream_info.channels,
                           md->data.stream_info.total_samples);
}

// ── 以下是 mpg123 的自定义 reader ──
// 原来用 mpg123_open_feed（往解码器里塞字节），那个模式文件是不可回退的，
// mpg123_seek 一律返回错误，所以进度条拖了没反应。
// 换成 handle 模式：mpg123 主动向我们要字节、要位置，seek 就变成一次 lseek。
mpg123_ssize_t mp3_read_cb(void* handle, void* buf, size_t count) {
    auto* e = static_cast<AudioEngine*>(handle);
    bool eof = false;
    size_t n = e->impl_pull(static_cast<uint8_t*>(buf), count, &eof);
    return (mpg123_ssize_t)n;   // 0 = EOF；没数据时 impl_pull 内部会等下载
}

off_t mp3_seek_cb(void* handle, off_t offset, int whence) {
    auto* e = static_cast<AudioEngine*>(handle);
    long long base = 0;
    if (whence == SEEK_CUR) {
        base = (long long)e->impl_tell();
    } else if (whence == SEEK_END) {
        const uint64_t total = e->impl_total_bytes();
        if (total == 0) return (off_t)-1;
        base = (long long)total;
    } else if (whence != SEEK_SET) {
        return (off_t)-1;
    }
    const long long target = base + (long long)offset;
    if (target < 0) return (off_t)-1;
    if (!e->impl_seek_bytes((uint64_t)target)) return (off_t)-1;
    return (off_t)target;
}

void mp3_cleanup_cb(void*) {}

}  // namespace

// ─────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────
void AudioEngine::impl_set_streaminfo(unsigned rate, unsigned channels, uint64_t total_samples) {
    if (rate > 0) src_rate_ = rate;
    if (channels > 0) src_channels_ = channels;
    if (total_samples > 0) src_frames_ = total_samples;
    if (src_rate_ > 0 && src_frames_ > 0)
        duration_sec_.store((double)src_frames_ / (double)src_rate_);
}

AudioEngine& AudioEngine::get() {
    static AudioEngine e;
    return e;
}

AudioEngine::AudioEngine() {
    impl_ = new Impl();
    impl_->ring.resize((size_t)kRingFrames * kOutChannels);
    static bool mpg123_ready = false;
    if (!mpg123_ready) {
        mpg123_init();
        mpg123_ready = true;
    }
}

AudioEngine::~AudioEngine() { stop_output(); }

// ─────────────────────────────────────────────────────────────
// 下载线程
// ─────────────────────────────────────────────────────────────
void AudioEngine::download_loop(std::string url) {
    CURL* c = curl_easy_init();
    if (!c) {
        std::lock_guard<std::mutex> lk(buf_mu_);
        dl_failed_ = true;
        dl_done_ = true;
        buf_cv_.notify_all();
        return;
    }

    DlSink sink;
    sink.buf = &buf_;
    sink.mu = &buf_mu_;
    sink.cv = &buf_cv_;
    sink.abort = &dl_abort_;
    sink.failed = &dl_failed_;
    sink.total = &dl_total_;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Referer: https://y.qq.com/");
    hdrs = curl_slist_append(hdrs, "Origin: https://y.qq.com");
    hdrs = curl_slist_append(hdrs, "Accept: */*");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // 默认 16KB 一块，一首无损要回调两千多次，每次都抢 buf_mu_。
    // 加到 64KB，回调次数降到 1/4，解码线程被挡在锁外的概率同步下降。
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
    // 流式：拿不到 Content-Length 也要继续
    curl_easy_setopt(c, CURLOPT_HTTP09_ALLOWED, 1L);
#ifdef __SWITCH__
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
#endif

    CURLcode rc = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    {
        std::lock_guard<std::mutex> lk(buf_mu_);
        if (sink.overflow) {
            dl_failed_ = true;
            last_error_ = "audio stream exceeds buffer cap";
        } else if (rc != CURLE_OK && !dl_abort_.load()) {
            dl_failed_ = true;
            last_error_ = std::string("stream failed: ") + curl_easy_strerror(rc);
        } else if (code != 200 && code != 206) {
            dl_failed_ = true;
            last_error_ = "stream HTTP " + std::to_string(code);
        }
        dl_done_ = true;
    }
    buf_cv_.notify_all();
}

void AudioEngine::abort_download() {
    dl_abort_.store(true);
    buf_cv_.notify_all();
    if (dl_thread_.joinable()) dl_thread_.join();
    dl_abort_.store(false);
}

bool AudioEngine::wait_bytes(size_t n) {
    std::unique_lock<std::mutex> lk(buf_mu_);
    while (buf_.size() < n && !dl_done_ && !dl_abort_.load() && !dl_failed_) {
        buf_cv_.wait_for(lk, std::chrono::milliseconds(100));
    }
    if (dl_failed_) return false;
    return buf_.size() >= n || dl_done_;
}

// 预缓冲用：等到够数、下载结束、或超时。超时不算失败——有多少先播多少，
// 总比让用户盯着"加载中"转圈强。
bool AudioEngine::wait_bytes_until(size_t n, int timeout_ms) {
    std::unique_lock<std::mutex> lk(buf_mu_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (buf_.size() < n && !dl_done_ && !dl_abort_.load() && !dl_failed_) {
        if (buf_cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    return buf_.size() >= n;
}

// 等下载线程跑完。m4a 的 moov 常在文件尾，不整首下完就没法解析样本表。
// 注意：这里必须用 wait 释放 buf_mu_，否则下载线程永远追加不进新字节 → 死等。
bool AudioEngine::wait_download_done(int timeout_ms) {
    std::unique_lock<std::mutex> lk(buf_mu_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!dl_done_ && !dl_abort_.load() && !dl_failed_) {
        if (buf_cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    return dl_done_ && !dl_failed_;
}

size_t AudioEngine::impl_pull(uint8_t* dst, size_t want, bool* eof) {
    if (eof) *eof = false;

    std::unique_lock<std::mutex> lk(buf_mu_);
    // 数据不够就等下载（这就是"边下边播"的背压点）
    while (read_pos_ >= buf_.size() && !dl_done_ && !dl_abort_.load()) {
        buf_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }

    if (read_pos_ >= buf_.size()) {
        if (eof) *eof = true;
        return 0;
    }
    if (!dst || want == 0) return buf_.size() - read_pos_;

    size_t avail = buf_.size() - read_pos_;
    size_t n = std::min(want, avail);
    std::memcpy(dst, buf_.data() + read_pos_, n);
    read_pos_ += n;
    return n;
}

bool AudioEngine::impl_seek_bytes(uint64_t offset) {
    std::unique_lock<std::mutex> lk(buf_mu_);
    // 顺序下载下，往后跳 = 等它下载到那儿
    while (buf_.size() < offset && !dl_done_ && !dl_abort_.load()) {
        buf_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
    if (buf_.size() < offset) return false;
    read_pos_ = (size_t)offset;
    return true;
}

uint64_t AudioEngine::impl_tell() const { return (uint64_t)read_pos_; }

uint64_t AudioEngine::impl_total_bytes() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(buf_mu_));
    return dl_total_ ? (uint64_t)dl_total_ : (uint64_t)buf_.size();
}

void AudioEngine::impl_fail(const std::string& msg) {
    if (last_error_.empty()) last_error_ = msg;
}

// ─────────────────────────────────────────────────────────────
// 重采样 + 写环形缓冲
// ─────────────────────────────────────────────────────────────
void AudioEngine::resample_push(const float* lr, size_t frames) {
    if (!impl_ || frames == 0 || src_rate_ == 0) return;
    const double step = (double)src_rate_ / (double)kOutRate;

    std::vector<int16_t>& dst = dst_buf_;   // 复用，别每帧 new
    dst.clear();
    const size_t need = (size_t)((frames + 1) / step + 2) * 2;
    if (dst.capacity() < need) dst.reserve(need);

    double t = rs_t_;
    while (t < (double)frames) {
        int i0 = (int)std::floor(t);
        float frac = (float)(t - (double)i0);

        float a_l, a_r, b_l, b_r;
        if (i0 < 0) {
            a_l = rs_prev_l_;
            a_r = rs_prev_r_;
        } else {
            a_l = lr[(size_t)i0 * 2];
            a_r = lr[(size_t)i0 * 2 + 1];
        }
        int i1 = i0 + 1;
        if (i1 < 0) {
            b_l = rs_prev_l_;
            b_r = rs_prev_r_;
        } else if (i1 >= (int)frames) {
            b_l = lr[(frames - 1) * 2];
            b_r = lr[(frames - 1) * 2 + 1];
        } else {
            b_l = lr[(size_t)i1 * 2];
            b_r = lr[(size_t)i1 * 2 + 1];
        }

        dst.push_back(to_s16(a_l + (b_l - a_l) * frac));
        dst.push_back(to_s16(a_r + (b_r - a_r) * frac));
        t += step;
    }
    rs_prev_l_ = lr[(frames - 1) * 2];
    rs_prev_r_ = lr[(frames - 1) * 2 + 1];
    rs_t_ = t - (double)frames;

    if (!dst.empty()) ring_write(impl_, dst.data(), dst.size() / 2);
}

void AudioEngine::impl_push_i32(const int32_t* const* ch, unsigned channels, size_t frames,
                                unsigned bits) {
    if (seeking_.load()) return;   // seek 过程中的试探解码丢掉，否则会冒出杂音
    if (channels == 0 || frames == 0) return;

    float scale = (bits > 1) ? 1.0f / (float)(1u << (bits - 1)) : 1.0f;
    lr_buf_.resize(frames * 2);
    float* lr = lr_buf_.data();
    for (size_t i = 0; i < frames; ++i) {
        float l = (float)ch[0][i] * scale;
        float r = (channels >= 2) ? (float)ch[1][i] * scale : l;
        lr[i * 2] = l;
        lr[i * 2 + 1] = r;
    }
    src_pos_.fetch_add(frames);
    resample_push(lr, frames);
}

void AudioEngine::impl_push_s16(const int16_t* interleaved, unsigned channels, size_t frames) {
    if (seeking_.load()) return;
    if (channels == 0 || frames == 0) return;

    std::vector<float>& lr = lr_buf_;   // 复用
    lr.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        float l = interleaved[i * channels] / 32768.0f;
        float r = (channels >= 2) ? interleaved[i * channels + 1] / 32768.0f : l;
        lr[i * 2] = l;
        lr[i * 2 + 1] = r;
    }
    src_pos_.fetch_add(frames);
    resample_push(lr.data(), frames);
}

// ─────────────────────────────────────────────────────────────
// 打开解码器
// ─────────────────────────────────────────────────────────────
bool AudioEngine::open_flac(std::string* error) {
    FLAC__StreamDecoder* d = FLAC__stream_decoder_new();
    if (!d) {
        *error = "flac decoder alloc failed";
        return false;
    }
    flac_ = d;

    FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_STREAMINFO);

    FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
        d, flac_read_cb, flac_seek_cb, flac_tell_cb, flac_length_cb, flac_eof_cb, flac_write_cb,
        flac_meta_cb, flac_error_cb, this);

    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        *error = std::string("flac init failed: ") +
                 FLAC__StreamDecoderInitStatusString[st];
        FLAC__stream_decoder_delete(d);
        flac_ = nullptr;
        return false;
    }

    // init_stream 与 init_FILE 不同：它**不会**自动读元数据。
    // 不显式推进到 metadata 结束，get_sample_rate() 永远是 0（"streaminfo missing" 就是这么来的）。
    if (!FLAC__stream_decoder_process_until_end_of_metadata(d)) {
        *error = std::string("flac metadata failed: ") +
                 FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(d)];
        FLAC__stream_decoder_finish(d);
        FLAC__stream_decoder_delete(d);
        flac_ = nullptr;
        return false;
    }

    // ① 先按规范自己解析 STREAMINFO —— 最可靠
    {
        unsigned r = 0, c = 0;
        uint64_t total = 0;
        std::lock_guard<std::mutex> lk(buf_mu_);
        if (parse_streaminfo(buf_.data(), buf_.size(), &r, &c, &total))
            impl_set_streaminfo(r, c, total);
    }
    // ② 还缺就用库的 accessor 补（不同版本行为不一致，只作兜底）
    if (src_rate_ == 0) src_rate_ = FLAC__stream_decoder_get_sample_rate(d);
    if (src_channels_ == 0) src_channels_ = FLAC__stream_decoder_get_channels(d);
    if (src_frames_ == 0) src_frames_ = FLAC__stream_decoder_get_total_samples(d);
    kind_ = 1;
    can_seek_.store(true);

    if (src_rate_ == 0 || src_channels_ == 0) {
        *error = "flac streaminfo missing";
        return false;
    }
    if (src_frames_ > 0) duration_sec_.store((double)src_frames_ / (double)src_rate_);
    return true;
}

bool AudioEngine::open_mp3(std::string* error) {
    int err = MPG123_OK;
    mpg123_handle* mh = mpg123_new(nullptr, &err);
    if (!mh) {
        *error = std::string("mpg123 alloc failed: ") + mpg123_plain_strerror(err);
        return false;
    }

    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    // 用自定义 reader 取代 feed 模式 —— 这是 seek 能工作的前提
    mpg123_replace_reader_handle(mh, mp3_read_cb, mp3_seek_cb, mp3_cleanup_cb);

    // open_handle 会立刻读并解析第一帧；数据还没到时 read_cb 会等下载线程
    if (mpg123_open_handle(mh, this) != MPG123_OK) {
        mpg123_delete(mh);
        *error = "mpg123 open_handle failed";
        return false;
    }

    long rate = 0;
    int ch = 0, enc = 0;
    if (mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK) {
        if (rate > 0) src_rate_ = (uint32_t)rate;
        if (ch > 0) src_channels_ = (uint32_t)ch;
    }
    if (src_rate_ == 0) src_rate_ = 44100;
    if (src_channels_ == 0) src_channels_ = 2;

    mp3_ = mh;
    kind_ = 2;
    can_seek_.store(true);   // handle 模式可以 seek 了

    // 时长：mpg123_length() 对 CBR 直接按文件长度算，很准。
    // VBR 上它可能返回 -1，那就交给解码循环用首帧码率估。
    const off_t len = mpg123_length(mh);
    if (len > 0 && src_rate_ > 0) {
        const double sec = (double)len / (double)src_rate_;
        if (sec > 0.5) duration_sec_.store(sec);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// AAC（faad2）：MP4 容器 / ADTS 裸流
// ─────────────────────────────────────────────────────────────
bool AudioEngine::init_faad(const std::vector<uint8_t>& asc, unsigned* rate, unsigned* ch,
                            std::string* error)
{
    NeAACDecHandle h = NeAACDecOpen();
    if (!h) {
        *error = "faad2: open failed";
        return false;
    }

    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    if (cfg) {
        cfg->defObjectType = LC;
        cfg->defSampleRate  = 44100;
        cfg->outputFormat   = FAAD_FMT_16BIT;   // 直接出 S16，省一次全量转换
        cfg->downMatrix     = 1;                // 多声道折叠成立体声，和输出端一致
        NeAACDecSetConfiguration(h, cfg);
    }

    unsigned long sr    = 0;
    unsigned char chans = 0;
    // Init2 用 AudioSpecificConfig 初始化：这是 MP4 里唯一可靠的参数来源，
    // 比从码流里猜稳得多（猜错的表现是全程噪音）。
    const char rc = NeAACDecInit2(h, const_cast<unsigned char*>(asc.data()),
                                  (unsigned long)asc.size(), &sr, &chans);
    if (rc != 0) {
        NeAACDecClose(h);
        *error = "faad2: AudioSpecificConfig 无效";
        return false;
    }

    aac_ = h;
    if (rate) *rate = (unsigned)sr;
    if (ch) *ch = (unsigned)chans;
    return true;
}

bool AudioEngine::open_aac(bool mp4, std::string* error)
{
    if (mp4) {
        Mp4Parse st = Mp4Parse::NeedMore;
        std::string perr;
        {
            std::lock_guard<std::mutex> lk(buf_mu_);
            st = mp4_parse_audio(buf_.data(), buf_.size(), &mp4_, &perr);
        }
        if (st == Mp4Parse::Error) {
            *error = perr.empty() ? "m4a 解析失败" : perr;
            return false;
        }
        if (st == Mp4Parse::NeedMore) {
            // moov 写在文件尾（QQ 音乐的 m4a 基本都是这样，没做 faststart）。
            // 流式拿不到样本表，只能等整首下完再解析 —— m4a 通常 3~10MB，
            // 几百毫秒到几秒，界面上显示"正在缓冲"。
            if (!wait_download_done(45000)) {
                *error = dl_failed_ ? (last_error_.empty() ? "m4a 下载失败" : last_error_)
                                    : "m4a 缓冲超时";
                return false;
            }
            std::lock_guard<std::mutex> lk(buf_mu_);
            st = mp4_parse_audio(buf_.data(), buf_.size(), &mp4_, &perr);
            if (st != Mp4Parse::Ok) {
                *error = perr.empty() ? "m4a 解析失败" : perr;
                return false;
            }
        }

        unsigned r = 0, c = 0;
        if (!init_faad(mp4_.asc, &r, &c, error)) return false;

        aac_pack_     = 1;
        aac_idx_      = 0;
        src_rate_     = r ? r : mp4_.sample_rate;
        src_channels_ = c ? c : mp4_.channels;
        if (src_rate_ == 0) src_rate_ = 44100;
        if (src_channels_ == 0) src_channels_ = 2;

        const double dur = mp4_.duration_sec();
        if (dur > 0.5) duration_sec_.store(dur);

        can_seek_.store(true);   // 有样本表 + 数据全在内存，seek 比 MP3 还准
        kind_ = 3;
        return true;
    }

    // —— ADTS 裸流 ——
    NeAACDecHandle h = NeAACDecOpen();
    if (!h) {
        *error = "faad2: open failed";
        return false;
    }
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    if (cfg) {
        cfg->defObjectType = LC;
        cfg->outputFormat  = FAAD_FMT_16BIT;
        cfg->downMatrix    = 1;
        NeAACDecSetConfiguration(h, cfg);
    }

    unsigned long sr    = 0;
    unsigned char chans = 0;
    long used           = -1;
    {
        std::lock_guard<std::mutex> lk(buf_mu_);
        if (buf_.size() < 32) {
            NeAACDecClose(h);
            *error = "aac 数据不足";
            return false;
        }
        used = NeAACDecInit(h, buf_.data(), (unsigned long)buf_.size(), &sr, &chans);
        if (used > 0) read_pos_ = (size_t)used;
    }
    if (used < 0) {
        NeAACDecClose(h);
        *error = "faad2: ADTS 头无效";
        return false;
    }

    aac_          = h;
    aac_pack_     = 2;
    src_rate_     = sr ? (uint32_t)sr : 44100;
    src_channels_ = chans ? (uint32_t)chans : 2;
    can_seek_.store(false);   // ADTS 没有索引表，跳转只能靠重扫，先不做
    kind_ = 3;
    return true;
}

void AudioEngine::close_decoder() {
    if (kind_ == 1 && flac_) {
        FLAC__stream_decoder_finish((FLAC__StreamDecoder*)flac_);
        FLAC__stream_decoder_delete((FLAC__StreamDecoder*)flac_);
        flac_ = nullptr;
    }
    if (kind_ == 2 && mp3_) {
        mpg123_close((mpg123_handle*)mp3_);
        mpg123_delete((mpg123_handle*)mp3_);
        mp3_ = nullptr;
    }
    if (kind_ == 3 && aac_) {
        NeAACDecClose((NeAACDecHandle)aac_);
        aac_ = nullptr;
    }
    kind_ = 0;
    aac_pack_ = 0;
    aac_idx_ = 0;
    mp4_ = Mp4Audio{};
    src_pos_.store(0);
    mp3_fed_ = 0;
    rs_t_ = 0.0;
    rs_prev_l_ = rs_prev_r_ = 0.0f;
}

// ─────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────
bool AudioEngine::load(const std::string& url, std::string* error) {
    std::string dummy;
    if (!error) error = &dummy;

    // 每次播放一律从零开始：先释放上一首的内存缓冲 / 线程 / 解码器，
    // 再重新向服务器请求。不做任何本地复用，也不落盘 —— 所以永远没有"旧缓存"。
    unload();
    state_.store(PlayState::Loading);

    dl_thread_ = std::thread(&AudioEngine::download_loop, this, url);

    // 先攒一点数据再判断格式；32KB 足够拿到 FLAC 的 STREAMINFO
    if (!wait_bytes(32 * 1024)) {
        abort_download();
        *error = last_error_.empty() ? "stream open failed" : last_error_;
        state_.store(PlayState::Failed);
        return false;
    }

    const char* codec;
    bool empty_resp = false;
    {
        std::lock_guard<std::mutex> lk(buf_mu_);
        if (buf_.empty()) {
            // 不能在锁内 abort_download()：join 下载线程时它会去抢 buf_mu_ → 死锁
            empty_resp = true;
        } else {
            codec = sniff_codec(buf_.data(), buf_.size());
            dl_total_ = dl_total_ ? dl_total_ : buf_.size();
            info_.bytes = dl_total_;
        }
    }
    if (empty_resp) {
        abort_download();          // 不 join 掉线程，退出时 std::thread 析构会 terminate
        *error = "empty response";
        state_.store(PlayState::Failed);
        return false;
    }
    // 状态栏给用户看的名字：「M4A」是容器不是编码，写 AAC 更准确
    info_.codec = (std::strcmp(codec, "M4A") == 0) ? "AAC" : codec;

    // 一次性把整首歌要用的容量占好。不这么做的话 vector 会随下载反复扩容，
    // 每次扩容都要在 buf_mu_ 锁内把已下载的全部字节再拷一遍——
    // 一首 40MB 的无损累计要拷上百 MB，而这段时间解码线程被挡在锁外，
    // 表现就是"越往后越卡"。这是本轮卡顿的二号原因。
    {
        std::lock_guard<std::mutex> lk(buf_mu_);
        size_t cap = dl_total_ ? dl_total_ : (32u * 1024 * 1024);
        if (cap > kMaxAudioBytes) cap = kMaxAudioBytes;
        if (buf_.capacity() < cap) {
            try {
                buf_.reserve(cap);
            } catch (const std::bad_alloc&) {
                try { buf_.reserve(8u * 1024 * 1024); } catch (...) {}
            }
        }
    }

    if (std::strcmp(codec, "FLAC") == 0) {
        if (!open_flac(error)) {
            abort_download();
            state_.store(PlayState::Failed);
            return false;
        }
    } else if (std::strcmp(codec, "MP3") == 0) {
        if (!open_mp3(error)) {
            abort_download();
            state_.store(PlayState::Failed);
            return false;
        }
    } else if (std::strcmp(codec, "M4A") == 0) {
        if (!open_aac(true, error)) {
            abort_download();
            state_.store(PlayState::Failed);
            return false;
        }
    } else if (std::strcmp(codec, "AAC") == 0) {
        if (!open_aac(false, error)) {
            abort_download();
            state_.store(PlayState::Failed);
            return false;
        }
    } else {
        *error = std::string("unsupported codec: ") + codec + " (FLAC/MP3/AAC only)";
        abort_download();
        state_.store(PlayState::Failed);
        return false;
    }

    // —— 预缓冲：攒够 3.5 秒的源数据再交给界面去播 ——
    // 放在这里而不是 play()：load() 跑在后台线程，等多久都不会卡住界面。
    {
        size_t target = 512 * 1024;
        const double dur = duration_sec_.load();
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lk(buf_mu_);
            total = dl_total_;
        }
        if (dur > 1.0 && total > 0) {
            const double per_sec = (double)total / dur;   // 每秒源字节数
            target = (size_t)(per_sec * kPrebufferSecs);
        }
        if (target < kPrebufferMin) target = kPrebufferMin;
        if (target > kPrebufferMax) target = kPrebufferMax;
        wait_bytes_until(target, kPrebufferWaitMs);
        if (dl_failed_) {
            abort_download();
            *error = last_error_.empty() ? "stream failed during prebuffer" : last_error_;
            state_.store(PlayState::Failed);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(info_mu_);
        info_.sample_rate = (int)src_rate_;
        info_.channels = (int)src_channels_;
        info_.duration_sec = duration_sec_.load();
    }

    state_.store(PlayState::Ready);
    return true;
}

float AudioEngine::load_progress() const {
    size_t total;
    size_t got;
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(buf_mu_));
        total = dl_total_;
        got = buf_.size();
    }
    if (total == 0) return 1.0f;   // 未知长度就当已经完整
    float v = (float)((double)got / (double)total);
    return v > 1.0f ? 1.0f : v;
}

// ─────────────────────────────────────────────────────────────
// 解码线程
// ─────────────────────────────────────────────────────────────
void AudioEngine::feed_thread_entry(void* arg) { static_cast<AudioEngine*>(arg)->feed_loop(); }

void AudioEngine::feed_loop() {
    Impl* im = impl_;

    if (kind_ == 1) {
        FLAC__StreamDecoder* d = (FLAC__StreamDecoder*)flac_;
        while (!im->abort) {
            long long want = pending_seek_.exchange(-1);
            if (want >= 0) {
                seeking_.store(true);
                {
                    std::lock_guard<std::mutex> lk(im->mu);
                    im->head = 0;
                    im->size = 0;
                }
                FLAC__stream_decoder_seek_absolute(d, (FLAC__uint64)want);
                seeking_.store(false);
                src_pos_.store((uint64_t)want);
                continue;
            }
            if (im->paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!FLAC__stream_decoder_process_single(d)) break;
            if (FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
        }
    } else if (kind_ == 2) {
        mpg123_handle* mh = (mpg123_handle*)mp3_;
        std::vector<uint8_t> out((size_t)kOutRate * 2 * 2);   // 0.5 秒余量

        // VBR 的时长只能估算（首帧码率 + Content-Length）；
        // CBR 已经在 open_mp3 里用 mpg123_length 算准了，这里只兜底。
        auto update_dur = [&]() {
            if (duration_sec_.load() > 0.0) return;   // 估一次就定，避免 VBR 让进度条跳
            if (dl_total_ == 0 || src_rate_ == 0) return;
            struct mpg123_frameinfo fi;
            if (mpg123_info(mh, &fi) != MPG123_OK || fi.bitrate <= 0) return;
            double sec = (double)dl_total_ * 8.0 / ((double)fi.bitrate * 1000.0);
            if (sec <= 0.5) return;
            duration_sec_.store(sec);
            std::lock_guard<std::mutex> lk(info_mu_);
            info_.duration_sec = sec;
            info_.bytes = dl_total_;
        };

        while (!im->abort) {
            // seek：pending_seek_ 存的是**样本帧**位置，不是秒也不是字节
            long long want = pending_seek_.exchange(-1);
            if (want >= 0) {
                seeking_.store(true);
                {
                    std::lock_guard<std::mutex> lk(im->mu);
                    im->head = 0;
                    im->size = 0;
                }
                off_t got = mpg123_seek(mh, (off_t)want, SEEK_SET);
                seeking_.store(false);
                if (got >= 0) {
                    src_pos_.store((uint64_t)got);
                } else {
                    last_error_ = std::string("mpg123 seek failed: ") +
                                  mpg123_plain_strerror(mpg123_errcode(mh));
                }
                continue;
            }
            if (im->paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // handle 模式下没数据时 mpg123_read 自己会卡在 read_cb 里等下载，
            // 不需要原来的 feed 缓冲和 NEED_MORE 轮询。
            size_t done = 0;
            const int rc = mpg123_read(mh, out.data(), out.size(), &done);

            if (done >= sizeof(int16_t) && src_channels_ > 0) {
                impl_push_s16((const int16_t*)out.data(), src_channels_,
                              done / (2 * src_channels_));
            }

            if (rc == MPG123_NEW_FORMAT) {
                long rate = 0;
                int ch = 0, enc = 0;
                if (mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK && rate > 0) {
                    src_rate_ = (uint32_t)rate;
                    src_channels_ = (uint32_t)ch;
                    std::lock_guard<std::mutex> lk(info_mu_);
                    info_.sample_rate = (int)rate;
                    info_.channels = ch;
                }
            } else if (rc == MPG123_DONE) {
                break;
            } else if (rc == MPG123_ERR) {
                impl_fail(std::string("mpg123: ") + mpg123_plain_strerror(mpg123_errcode(mh)));
                break;
            }
            update_dur();
        }

        update_dur();
    } else if (kind_ == 3) {
        NeAACDecHandle h = (NeAACDecHandle)aac_;
        std::vector<uint8_t> fed;   // 真正喂进解码器的字节（MP4 那条路要补 ADTS 头）

        while (!im->abort) {
            // —— seek：pending_seek_ 是目标采样帧位置 ——
            long long want = pending_seek_.exchange(-1);
            if (want >= 0) {
                seeking_.store(true);
                {
                    std::lock_guard<std::mutex> lk(im->mu);
                    im->head = 0;
                    im->size = 0;
                }
                const double sec = (double)want / (double)(src_rate_ ? src_rate_ : 44100);
                if (aac_pack_ == 1 && !mp4_.samples.empty()) {
                    aac_idx_ = mp4_.sample_at(sec);
                    // 重置解码器的重叠/预测状态，否则跳转后头一两帧会有杂音
                    NeAACDecPostSeekReset(h, (long)aac_idx_);
                    const double t0 = aac_idx_ < mp4_.starts.size() ? mp4_.starts[aac_idx_] : sec;
                    src_pos_.store((uint64_t)(t0 * (double)src_rate_));
                }
                seeking_.store(false);
                continue;
            }
            if (im->paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (aac_pack_ == 1) {
                // MP4：按样本表逐帧取
                if (aac_idx_ >= mp4_.samples.size()) break;
                const Mp4Sample& s = mp4_.samples[aac_idx_];
                if (s.size == 0) {
                    ++aac_idx_;
                    continue;
                }
                // faststart 的文件 mdat 可能还没下完 → 这里会等下载
                if (!impl_seek_bytes(s.offset)) break;
                fed.resize(7 + (size_t)s.size);
                bool eof = false;
                if (impl_pull(fed.data() + 7, s.size, &eof) < s.size) break;
                build_adts_header(mp4_.asc, s.size, fed.data());
                ++aac_idx_;
            } else {
                // ADTS：从缓冲里凑出完整的一帧（含头）
                size_t flen = 0;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lk(buf_mu_);
                        if (read_pos_ + 7 <= buf_.size())
                            flen = adts_frame_len(buf_.data() + read_pos_, buf_.size() - read_pos_);
                    }
                    if (flen > 0) break;
                    if (dl_done_ || dl_abort_.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (flen == 0) break;
                fed.resize(flen);
                bool eof = false;
                if (impl_pull(fed.data(), flen, &eof) < flen) break;
            }

            NeAACDecFrameInfo fi;
            std::memset(&fi, 0, sizeof(fi));
            void* out = NeAACDecDecode(h, &fi, fed.data(), (unsigned long)fed.size());
            if (out && fi.error == 0 && fi.channels > 0 && fi.samples > 0) {
                // faad2 的 samples 是**交错后的总样本数**，要除以声道数才是帧数。
                // 弄错这里，进度条会以两倍速跑。
                const size_t frames = (size_t)(fi.samples / fi.channels);
                if (fi.samplerate && fi.samplerate != src_rate_) {
                    // HE-AAC 的输出率可能是 ASC 核心率的两倍，以解码器实际值为准
                    src_rate_ = (uint32_t)fi.samplerate;
                    std::lock_guard<std::mutex> lk(info_mu_);
                    info_.sample_rate = (int)src_rate_;
                }
                impl_push_s16((const int16_t*)out, fi.channels, frames);
            }
            // 单帧 error 不算致命（m4a 开头的 priming 帧常报错），继续解下一帧

            if (aac_pack_ == 2 && duration_sec_.load() <= 0.0 && dl_total_ > 0 && fi.samples > 0) {
                // ADTS 没有样本表：用首帧的「字节/时长」推总时长，只估一次
                const double frame_sec = (double)(fi.samples / fi.channels) /
                                         (double)(fi.samplerate ? fi.samplerate : src_rate_);
                if (frame_sec > 0.0 && fi.bytesconsumed > 0) {
                    const double est = ((double)dl_total_ / (double)fi.bytesconsumed) * frame_sec;
                    if (est > 0.5) {
                        duration_sec_.store(est);
                        std::lock_guard<std::mutex> lk(info_mu_);
                        info_.duration_sec = est;
                    }
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->eof = true;
        im->cv_data.notify_all();
    }
}

// ─────────────────────────────────────────────────────────────
// 输出
// ─────────────────────────────────────────────────────────────
#ifdef __SWITCH__
void AudioEngine::start_output() {
    Impl* im = impl_;
    if (im->out_ready) return;

    if (R_FAILED(audoutInitialize())) {
        last_error_ = "audoutInitialize failed";
        state_.store(PlayState::Failed);
        return;
    }
    audoutStartAudioOut();

    const size_t buf_bytes = (size_t)kBufFrames * kOutChannels * sizeof(int16_t);
    for (int i = 0; i < kOutBufCount; ++i) {
        im->mem[i] = (int16_t*)memalign(0x1000, buf_bytes);   // audout 要页对齐
        if (!im->mem[i]) {
            last_error_ = "memalign failed for audio buffer";
            state_.store(PlayState::Failed);
            audoutStopAudioOut();
            audoutExit();
            return;
        }
        std::memset(im->mem[i], 0, buf_bytes);

        std::memset(&im->buf[i], 0, sizeof(AudioOutBuffer));
        im->buf[i].next = nullptr;
        im->buf[i].buffer = im->mem[i];
        im->buf[i].buffer_size = buf_bytes;
        im->buf[i].data_size = buf_bytes;
        im->buf[i].data_offset = 0;
    }

    im->abort = false;
    im->eof = false;
    im->head = 0;
    im->size = 0;
    im->out_ready = true;

    im->feeder = std::thread(&AudioEngine::feed_thread_entry, this);
    im->pump = std::thread(&AudioEngine::pump_thread_entry, this);

    for (int i = 0; i < kOutBufCount; ++i) {
        size_t n = ring_read(im, im->mem[i], kBufFrames);
        if (n == 0) {
            std::memset(im->mem[i], 0, buf_bytes);
            n = kBufFrames;
        }
        im->buf[i].data_size = n * kOutChannels * sizeof(int16_t);
        im->buf[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&im->buf[i]);
    }
}

void AudioEngine::stop_output() {
    Impl* im = impl_;
    if (!im || !im->out_ready) return;

    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->abort = true;
        im->cv_data.notify_all();
        im->cv_space.notify_all();
    }
    if (im->feeder.joinable()) im->feeder.join();
    if (im->pump.joinable()) im->pump.join();

    audoutStopAudioOut();
    audoutExit();

    for (int i = 0; i < kOutBufCount; ++i) {
        if (im->mem[i]) {
            free(im->mem[i]);
            im->mem[i] = nullptr;
        }
    }
    im->out_ready = false;
}

void AudioEngine::pump_thread_entry(void* arg) { static_cast<AudioEngine*>(arg)->pump_loop(); }

void AudioEngine::pump_loop() {
    Impl* im = impl_;
    while (!im->abort) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        Result rc = audoutWaitPlayFinish(&released, &count, 100000000ULL);
        if (R_FAILED(rc) || count == 0 || released == nullptr) continue;

        for (u32 i = 0; i < count && !im->abort; ++i) {
            AudioOutBuffer* b = &released[i];
            auto* mem = static_cast<int16_t*>(b->buffer);
            size_t frames = b->buffer_size / (kOutChannels * sizeof(int16_t));

            size_t n;
            if (im->paused) {
                std::memset(mem, 0, b->buffer_size);   // 暂停灌静音，不断流
                n = frames;
            } else {
                n = ring_read(im, mem, frames);
                if (n == 0) {
                    std::memset(mem, 0, b->buffer_size);
                    n = frames;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            b->data_size = n * kOutChannels * sizeof(int16_t);
            b->data_offset = 0;
            audoutAppendAudioOutBuffer(b);
        }
    }
}
#else
// PC：没有 audout，只是按实时速度把环形缓冲抽掉，用于验证解码/重采样
void AudioEngine::start_output() {
    Impl* im = impl_;
    if (im->out_ready) return;
    im->out_ready = true;
    im->abort = false;
    im->eof = false;
    im->head = 0;
    im->size = 0;
    im->feeder = std::thread(&AudioEngine::feed_thread_entry, this);
    im->pump = std::thread(&AudioEngine::pump_thread_entry, this);
}

void AudioEngine::stop_output() {
    Impl* im = impl_;
    if (!im || !im->out_ready) return;
    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->abort = true;
        im->cv_data.notify_all();
        im->cv_space.notify_all();
    }
    if (im->feeder.joinable()) im->feeder.join();
    if (im->pump.joinable()) im->pump.join();
    im->out_ready = false;
}

void AudioEngine::pump_thread_entry(void* arg) { static_cast<AudioEngine*>(arg)->pump_loop(); }

// PC 没有声卡：按 48kHz 实时速度把环形缓冲抽掉，模拟 audout 的消费节奏。
// 这样"下载 → 解码 → 重采样 → 环形缓冲 → 消费"整条链路在 PC 上跑得和 Switch 一样，
// 能在上机前就验证背压、seek、EOF，而不是每次等真机。
void AudioEngine::pump_loop() {
    Impl* im = impl_;
    const size_t per_tick = kOutRate / 20;   // 50ms 的帧量
    while (!im->abort) {
        {
            std::lock_guard<std::mutex> lk(im->mu);
            size_t take = std::min(per_tick, im->size);
            im->head = (im->head + take) % kRingFrames;
            im->size -= take;
            im->cv_space.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
#endif

// ─────────────────────────────────────────────────────────────
// 控制
// ─────────────────────────────────────────────────────────────
void AudioEngine::play() {
    PlayState s = state_.load();
    if (s == PlayState::Failed || s == PlayState::Idle) return;

    if (impl_->out_ready) {
        if (s == PlayState::Paused) {
            impl_->paused = false;
            state_.store(PlayState::Playing);
        }
        return;
    }
    impl_->paused = false;
    start_output();
    state_.store(PlayState::Playing);
}

void AudioEngine::pause() {
    if (state_.load() != PlayState::Playing) return;
    impl_->paused = true;
    state_.store(PlayState::Paused);
}

void AudioEngine::toggle() {
    if (state_.load() == PlayState::Playing) pause();
    else play();
}

void AudioEngine::stop() {
    impl_->paused = false;
    stop_output();
    if (kind_ == 1 && flac_) {
        FLAC__stream_decoder_seek_absolute((FLAC__StreamDecoder*)flac_, 0);
        src_pos_.store(0);
    }
    if (state_.load() != PlayState::Failed) state_.store(PlayState::Ready);
}

// 释放本次播放的全部资源。
// 本引擎从不写磁盘（没有 .tmp / 没有缓存目录），所以没有"缓存文件"需要删；
// 这里释放的是内存里的下载缓冲（一首无损最大几十 MB，Switch 内存紧张，必须还回去）。
void AudioEngine::unload() {
    impl_->paused = false;
    stop_output();      // 先停声，避免 pump 线程还在读环形缓冲
    abort_download();   // 断下载线程（join 之后再动 buf_ 才安全）
    close_decoder();

    {
        std::lock_guard<std::mutex> lk(buf_mu_);
        // swap 空 vector 才是真正 free；clear()+shrink_to_fit() 只保证逻辑容量
        std::vector<uint8_t>().swap(buf_);
        read_pos_ = 0;
        mp3_fed_ = 0;
        dl_done_ = false;
        dl_failed_ = false;
        dl_total_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        info_ = TrackInfo();
    }
    last_error_.clear();
    src_pos_.store(0);
    src_rate_ = 0;
    src_channels_ = 0;
    src_frames_ = 0;
    pending_seek_.store(-1);
    seeking_.store(false);
    can_seek_.store(false);
    duration_sec_.store(0.0);
    state_.store(PlayState::Idle);
}

void AudioEngine::seek(double sec) {
    // 1=FLAC（seek_absolute）、2=MP3（mpg123_seek）、3=AAC（按 MP4 样本表定位）
    // ADTS 裸流没有索引表，can_seek_ 一直是 false，这里直接拒绝。
    if (kind_ == 0 || !can_seek_.load()) return;
    double dur = duration_sec_.load();
    if (sec < 0) sec = 0;
    if (dur > 0 && sec > dur) sec = dur;
    pending_seek_.store((long long)(sec * (double)src_rate_));
}

double AudioEngine::position() const {
    if (src_rate_ == 0) return 0.0;
    uint64_t decoded = src_pos_.load();
    uint64_t buffered_48k = impl_ ? (uint64_t)impl_->size : 0;
    uint64_t buffered_src = buffered_48k * (uint64_t)src_rate_ / (uint64_t)kOutRate;
    uint64_t played = decoded > buffered_src ? decoded - buffered_src : 0;
    double p = (double)played / (double)src_rate_;
    double d = duration_sec_.load();
    if (d > 0 && p > d) p = d;
    return p;
}

TrackInfo AudioEngine::info() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return info_;
}

}  // namespace hd
