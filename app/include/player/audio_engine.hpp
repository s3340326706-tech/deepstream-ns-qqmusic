// audio_engine.hpp —— 播放引擎：边下边播（流式解码）+ 输出
//
// 设计要点（都是踩过坑之后的决定）：
//  1. 解码器按**文件头 magic** 选，不按 Content-Type。
//     QQ 的 CDN 对 FLAC 返回 audio/x-ogg，信 MIME 会拿 mp3 解码器解 FLAC。
//  2. 输出走 audout（48kHz/2ch/s16 固定），源多为 44.1kHz → 必须重采样。
//  3. 流式：下载线程把字节追加到内存缓冲，解码器从缓冲里拉数据（read 回调阻塞等下载）。
//     所以"播到哪下到哪"，不再等整首下完。
//     代价：FLAC 用 libFLAC（dr_flac 只能吃完整缓冲），MP3 用 mpg123 feed 模式。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "player/mp4_demux.hpp"

namespace hd {

enum class PlayState {
    Idle,       // 什么都没加载
    Loading,    // 正在连流 / 读头部
    Ready,      // 已就绪，未播放
    Playing,
    Paused,
    Failed,
};

struct TrackInfo {
    std::string title;
    std::string artist;
    std::string album;
    double duration_sec = 0;
    int sample_rate = 0;
    int channels = 0;
    std::string codec;      // "FLAC" / "MP3"
    size_t bytes = 0;
};

class AudioEngine {
public:
    static AudioEngine& get();
    ~AudioEngine();

    // 连流 + 读头部（不下载完整文件）。返回 true 表示可以立刻 play()。
    bool load(const std::string& url, std::string* error);

    void play();
    void pause();
    void toggle();
    void stop();

    // 彻底释放：停输出、断下载线程、关解码器、把下载缓冲真正还给系统。
    // 说明：本引擎**全程不写磁盘**（没有磁盘缓存文件可删），占用只存在于内存缓冲里，
    // 所以"清缓存"在这里等价于"释放缓冲"。每次换歌前自动调用，
    // 保证下一首一定是从服务器重新下载，绝不复用上一首的数据。
    void unload();
    void seek(double sec);

    double duration() const { return duration_sec_.load(); }
    double position() const;
    PlayState state() const { return state_.load(); }
    TrackInfo info() const;

    // 已下载比例（0..1），用于界面上的缓冲进度
    float load_progress() const;

    // MP3 的 feed 模式不支持回退，所以拖动进度只有 FLAC 能用
    bool can_seek() const { return can_seek_.load(); }

    const std::string& last_error() const { return last_error_; }

    struct Impl;   // 平台相关（Switch 下是 audout）

    // —— 内部接口：给 C 风格解码器回调用，外部不要调用 ——
    size_t impl_pull(uint8_t* dst, size_t want, bool* eof);
    bool impl_seek_bytes(uint64_t offset);
    uint64_t impl_tell() const;
    // 已下载总字节数（Content-Length 未知时返回目前已收到的量）。
    // mpg123 的 lseek(SEEK_END) 要用它。
    uint64_t impl_total_bytes() const;
    void impl_push_s16(const int16_t* interleaved, unsigned channels, size_t frames);
    void impl_push_i32(const int32_t* const* ch, unsigned channels, size_t frames, unsigned bits);
    void impl_fail(const std::string& msg);
    // FLAC STREAMINFO 回调：不走 get_sample_rate()，那条 accessor 有的版本返回 0
    void impl_set_streaminfo(unsigned rate, unsigned channels, uint64_t total_samples);

private:
    AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // 交错 LR float → 重采样到 48k → 环形缓冲
    void resample_push(const float* lr, size_t frames);
    double rs_t_ = 0.0;
    float rs_prev_l_ = 0.0f, rs_prev_r_ = 0.0f;
    // 解码线程专用暂存：复用而不是每帧 new 两个 vector。
    // 每 85ms 一次 malloc/free 看着无害，但 Switch 的堆有全局锁，
    // 碰上输出线程要内存时就会多一次抖动，正好叠在临界点上。
    std::vector<float> lr_buf_;
    std::vector<int16_t> dst_buf_;

    // —— 下载 ——
    void download_loop(std::string url);
    void abort_download();
    // 等缓冲里至少有 n 字节可读（或下载结束）；返回 false 表示中止/失败
    bool wait_bytes(size_t n);
    // 同上，但带超时：到点就返回当前够不够，用于预缓冲（网络慢不能把用户卡死在"加载中"）
    bool wait_bytes_until(size_t n, int timeout_ms);
    // 给解码器用：从 read_pos_ 读最多 *bytes
    size_t pull_bytes(uint8_t* dst, size_t want, bool* eof);

    // —— 解码 ——
    bool open_flac(std::string* error);
    bool open_mp3(std::string* error);
    // AAC：m4a(MP4 容器) 与 ADTS 裸流两种入口，内部都走 faad2
    bool open_aac(bool mp4, std::string* error);
    bool init_faad(const std::vector<uint8_t>& asc, unsigned* rate, unsigned* ch, std::string* error);
    void close_decoder();

    // 等下载线程跑完（m4a 的 moov 常在文件尾，必须等整首下完才能解析）
    bool wait_download_done(int timeout_ms);

    void start_output();
    void stop_output();
    static void feed_thread_entry(void* arg);
    static void pump_thread_entry(void* arg);
    void feed_loop();   // 解码 → 重采样 → 环形缓冲
    void pump_loop();   // 环形缓冲 → 输出设备

    // —— 下载缓冲 ——
    std::mutex buf_mu_;
    std::condition_variable buf_cv_;
    std::vector<uint8_t> buf_;     // 已下载字节，只增不减（seek 回退要能重读）
    size_t read_pos_ = 0;          // 解码器读位置
    bool dl_done_ = false;
    bool dl_failed_ = false;
    size_t dl_total_ = 0;          // Content-Length，未知为 0
    std::thread dl_thread_;
    std::atomic<bool> dl_abort_{false};

    // —— 解码状态 ——
    void* flac_ = nullptr;         // FLAC__StreamDecoder*
    void* mp3_ = nullptr;          // mpg123_handle*
    void* aac_ = nullptr;          // NeAACDecHandle
    int aac_pack_ = 0;             // AAC 来源：1=MP4 容器 2=ADTS 裸流
    Mp4Audio mp4_;                 // MP4 样本表（aac_pack_==1 时有效）
    size_t aac_idx_ = 0;           // 下一个要解的第几个样本
    std::vector<uint8_t> aac_frame_;   // 喂给 faad2 的帧（raw AAC 前面补 ADTS 头）
    int kind_ = 0;                 // 1=flac 2=mp3 3=aac
    uint32_t src_rate_ = 0;
    uint32_t src_channels_ = 0;
    uint64_t src_frames_ = 0;
    size_t mp3_fed_ = 0;           // 已喂给 mpg123 的字节数
    std::atomic<uint64_t> src_pos_{0};
    std::atomic<long long> pending_seek_{-1};
    std::atomic<bool> seeking_{false};
    std::atomic<bool> can_seek_{false};

    TrackInfo info_;
    std::string last_error_;

    // —— 输出状态 ——
    std::atomic<PlayState> state_{PlayState::Idle};
    std::atomic<double> duration_sec_{0.0};

    Impl* impl_ = nullptr;
    mutable std::mutex info_mu_;
};

}  // namespace hd
