// mp4_demux.hpp —— 从 MP4/M4A 容器里拆出 AAC 音轨
//
// 为什么需要这个：
//   QQ 音乐的「标准」音质里有一部分曲目只有 m4a（AAC），没有 mp3。
//   而 m4a 不是裸 AAC 流，是 ISOBMFF 容器（Box 结构），里面还混着
//   moov/mdat 这些与音频数据无关的块。faad2 只吃 AAC 帧，所以
//   必须先把容器拆开，拿到两样东西：
//     1. AudioSpecificConfig（ASC）—— 告诉解码器采样率/声道/对象类型
//     2. 每个 AAC 样本在文件里的 (偏移, 长度) —— 才能逐帧喂给解码器
//
// 只支持「单音轨 + mp4a/AAC」这一种情况，其他一律报错。
// 不做的事情：不处理视频轨、不处理分片 MP4(fMP4)、不支持加密。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hd {

struct Mp4Sample {
    uint64_t offset = 0;   // 相对文件头的绝对偏移
    uint32_t size   = 0;   // 字节数
};

struct Mp4Audio {
    uint32_t sample_rate = 0;   // 来自 ASC/mp4a（可能是 SBR 的核心采样率）
    uint32_t channels    = 0;
    uint32_t timescale   = 0;   // mdhd 的时间基（每秒的 tick 数）
    uint64_t duration_ts = 0;   // mdhd 的时长（tick 数）
    uint32_t frame_len   = 1024;   // 每个样本对应的 AAC 采样帧数（AAC-LC 恒为 1024）

    std::vector<uint8_t> asc;          // AudioSpecificConfig
    std::vector<Mp4Sample> samples;    // 样本表（按解码顺序）
    std::vector<uint32_t> durations;   // 每样本时长（timescale 单位），可为空

    // 每个样本的起始时间（秒），与 samples 等长。seek 时二分查找用。
    std::vector<double> starts;

    double duration_sec() const
    {
        if (timescale == 0) return 0.0;
        return (double)duration_ts / (double)timescale;
    }

    // 给定秒数，返回应该从第几个样本开始解码
    size_t sample_at(double sec) const;
};

enum class Mp4Parse {
    Ok,         // 解析成功
    NeedMore,   // 数据还不够（moov 在文件后面，得等下载）
    Error,      // 数据齐了但不是我们支持的 MP4
};

// data/size 是已经下载到的连续前缀。
// 返回 NeedMore 时调用方应当继续等待下载，攒够了再调一次。
Mp4Parse mp4_parse_audio(const uint8_t* data, size_t size, Mp4Audio* out, std::string* error);

// 给裸 AAC 帧补一个 7 字节 ADTS 头（无 CRC）。
// MP4 里存的是裸帧，而 faad2 靠 ADTS 头的 frame_length 找帧边界，
// 所以喂之前必须补。参数从 AudioSpecificConfig 推出来。
void mp4_build_adts_header(const uint8_t* asc, size_t asc_len, size_t payload, uint8_t out[7]);

}  // namespace hd
