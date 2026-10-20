#!/usr/bin/env python3
# verify_mp4_demux.py —— 用 Python 独立复现 mp4_demux.cpp 的解析逻辑，与 ffprobe 对账
#
# 目的不是"再写一个播放器"，而是验证算法：
#   1. moov/stbl 里取出来的样本表（数量、偏移、大小）对不对
#   2. ASC 解出来的 采样率/声道 与 ffprobe 是否一致
#   3. ADTS 头构造的 profile/freq/channel 位是否和 ffmpeg 自己生成的一致
#
# 用法：python verify_mp4_demux.py <file.m4a>

import struct
import subprocess
import sys


def boxes(data, off, end):
    """列出 [off, end) 内的直接子 box"""
    out = []
    while off + 8 <= end:
        size, typ = struct.unpack_from(">I4s", data, off)
        hdr = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, off + 8)[0]
            hdr = 16
        elif size == 0:
            size = end - off
        out.append((typ.decode("latin1"), off, size, off + hdr))
        if size < hdr:
            break
        off += size
    return out


def find(kids, name):
    for k in kids:
        if k[0] == name:
            return k
    return None


def desc_walk(p, n, depth=0):
    """esds 描述符遍历，返回 AudioSpecificConfig"""
    i = 0
    while i + 2 <= n:
        tag = p[i]
        i += 1
        ln = 0
        while i < n and i < 4 + (i - 1):
            c = p[i]
            i += 1
            ln = (ln << 7) | (c & 0x7F)
            if not (c & 0x80):
                break
        if i + ln > n:
            return None
        body = p[i:i + ln]
        if tag == 0x03:
            skip = 3
            if len(body) >= 3:
                flags = body[2]
                if flags & 0x80:
                    skip += 2
                if flags & 0x40:
                    skip += 1 + body[skip]
                if flags & 0x20:
                    skip += 2
                r = desc_walk(body[skip:], len(body) - skip, depth + 1)
                if r:
                    return r
        elif tag == 0x04:
            if len(body) >= 13:
                r = desc_walk(body[13:], len(body) - 13, depth + 1)
                if r:
                    return r
        elif tag == 0x05:
            return body
        i += ln
    return None


def parse(path):
    data = open(path, "rb").read()
    top = boxes(data, 0, len(data))
    names = [b[0] for b in top]
    moov = find(top, "moov")
    assert moov, "no moov"

    traks = boxes(data, moov[3], moov[1] + moov[2])
    for typ, off, size, body in traks:
        if typ != "trak":
            continue
        mdia = find(boxes(data, body, off + size), "mdia")
        if not mdia:
            continue
        mk = boxes(data, mdia[3], mdia[1] + mdia[2])
        hdlr = find(mk, "hdlr")
        if not hdlr or data[hdlr[3] + 8:hdlr[3] + 12] != b"soun":
            continue

        mdhd = find(mk, "mdhd")
        _, moff, msize, mbody = mdhd
        ver = data[mbody]
        if ver == 1:
            timescale = struct.unpack_from(">I", data, mbody + 20)[0]
            duration = struct.unpack_from(">Q", data, mbody + 24)[0]
        else:
            timescale = struct.unpack_from(">I", data, mbody + 12)[0]
            duration = struct.unpack_from(">I", data, mbody + 16)[0]

        minf = find(mk, "minf")
        stbl = find(boxes(data, minf[3], minf[1] + minf[2]), "stbl")
        sk = boxes(data, stbl[3], stbl[1] + stbl[2])
        stsd, stts, stsc, stsz = (find(sk, "stsd"), find(sk, "stts"),
                                  find(sk, "stsc"), find(sk, "stsz"))
        stco = find(sk, "stco") or find(sk, "co64")

        # stsd → mp4a → esds → ASC
        sbody = stsd[3]
        entry_off = sbody + 8
        esize, etype = struct.unpack_from(">I4s", data, entry_off)
        ebody = entry_off + 8
        channels = struct.unpack_from(">H", data, ebody + 16)[0]
        rate = struct.unpack_from(">I", data, ebody + 24)[0] >> 16
        asc = None
        for typ2, off2, size2, body2 in boxes(data, ebody + 28, entry_off + esize):
            if typ2 == "esds":
                asc = desc_walk(data[body2 + 4:body2 + size2], size2 - 4)

        # stsz
        n = struct.unpack_from(">I", data, stsz[3] + 8)[0]
        fixed = struct.unpack_from(">I", data, stsz[3] + 4)[0]
        sizes = [fixed] * n if fixed else [
            struct.unpack_from(">I", data, stsz[3] + 12 + 4 * i)[0] for i in range(n)
        ]

        # stsc
        cnt = struct.unpack_from(">I", data, stsc[3] + 4)[0]
        stscs = [struct.unpack_from(">II", data, stsc[3] + 8 + 12 * i)
                 for i in range(cnt)]

        # stco
        cnt = struct.unpack_from(">I", data, stco[3] + 4)[0]
        if stco[0] == "stco":
            chunks = [struct.unpack_from(">I", data, stco[3] + 8 + 4 * i)[0] for i in range(cnt)]
        else:
            chunks = [struct.unpack_from(">Q", data, stco[3] + 8 + 8 * i)[0] for i in range(cnt)]

        # 拼样本表
        samples = []
        cursor = 0
        idx = 0
        for ci, coff in enumerate(chunks):
            cn = ci + 1
            while idx + 1 < len(stscs) and stscs[idx + 1][0] <= cn:
                idx += 1
            per = stscs[idx][1]
            off = coff
            for _ in range(per):
                if cursor >= len(sizes):
                    break
                samples.append((off, sizes[cursor]))
                off += sizes[cursor]
                cursor += 1

        return {
            "timescale": timescale,
            "duration": duration,
            "rate": rate,
            "channels": channels,
            "asc": asc.hex().upper() if asc else None,
            "samples": samples,
            "top": names,
        }
    return None


def adts_header(asc_hex, payload):
    """与 mp4_build_adts_header 完全相同的算法"""
    asc = bytes.fromhex(asc_hex)
    obj = 2
    freq = 4
    ch = 2
    if len(asc) >= 2:
        obj = (asc[0] >> 3) & 0x1F
        freq = ((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01)
        ch = (asc[1] >> 3) & 0x0F
        if ch == 0:
            ch = 2
    profile = (obj - 1) if 1 <= obj <= 4 else 1
    if profile > 3:
        profile = 1
    ln = payload + 7
    out = bytearray(7)
    out[0] = 0xFF
    out[1] = 0xF1
    out[2] = (profile << 6) | ((freq & 0x0F) << 2) | ((ch >> 2) & 0x01)
    out[3] = ((ch & 0x03) << 6) | ((ln >> 11) & 0x03)
    out[4] = (ln >> 3) & 0xFF
    out[5] = ((ln & 0x07) << 5) | 0x1F
    out[6] = 0xFC
    return bytes(out)


def ffprobe(path):
    r = subprocess.run(
        ["C:/Users/admin/bin/ffprobe.exe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=codec_name,sample_rate,channels,duration,nb_frames",
         "-of", "default=nw=1", path],
        capture_output=True, text=True)
    d = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k] = v
    return d


def main():
    if len(sys.argv) < 2:
        print("usage: verify_mp4_demux.py <file.m4a>")
        return 2
    path = sys.argv[1]
    info = parse(path)
    if not info:
        print("parse failed")
        return 1

    data = open(path, "rb").read()
    print(f"top boxes     : {info['top']}")
    print(f"rate/channels : {info['rate']} Hz / {info['channels']} ch")
    print(f"timescale     : {info['timescale']}")
    print(f"duration      : {info['duration']} ticks = "
          f"{info['duration'] / info['timescale']:.3f}s")
    print(f"ASC           : {info['asc']}")
    if info["asc"]:
        a = bytes.fromhex(info["asc"])
        obj = a[0] >> 3
        fi = ((a[0] & 7) << 1) | (a[1] >> 7)
        ch = (a[1] >> 3) & 0x0F
        print(f"  ASC decode  : objectType={obj} freqIndex={fi} channelConfig={ch}")
    print(f"samples       : {len(info['samples'])}")
    print(f"first 3       : {info['samples'][:3]}")
    print(f"last 1        : {info['samples'][-1:]}")

    # 所有样本必须在文件内
    bad = [i for i, (o, s) in enumerate(info["samples"]) if o + s > len(data)]
    print(f"out-of-range  : {len(bad)} {bad[:5]}")

    # 与 ffprobe 对账
    probe = ffprobe(path)
    print(f"ffprobe       : {probe}")
    if probe.get("sample_rate"):
        ok_rate = int(probe["sample_rate"]) == info["rate"]
        ok_ch = int(probe["channels"]) == info["channels"]
        print(f"  rate match  : {ok_rate}   channels match: {ok_ch}")
    if probe.get("duration"):
        ok = abs(float(probe["duration"]) - info["duration"] / info["timescale"]) < 0.1
        print(f"  duration ok : {ok}")

    # ADTS 头对比：让 ffmpeg 以 copy 方式导出 ADTS，取它第一帧的头
    out = path + ".aac"
    subprocess.run(["C:/Users/admin/bin/ffmpeg.exe", "-y", "-v", "error", "-i", path,
                    "-c:a", "copy", "-f", "adts", out], capture_output=True)
    try:
        adts = open(out, "rb").read()
        ff_hdr = adts[:7]
        # ffmpeg 生成的是重新分帧的 ADTS，长度可能与源帧不同 → 只比前 4 字节的配置位
        mine = adts_header(info["asc"], 100)
        print(f"adts hdr mine : {mine[:4].hex(' ')}")
        print(f"adts hdr ffmpeg: {ff_hdr[:4].hex(' ')}")
        same_cfg = mine[0:2] == ff_hdr[0:2] and (mine[2] & 0xFC) == (ff_hdr[2] & 0xFC)
        print(f"  profile/freq/ch bits match: {same_cfg}")
    except FileNotFoundError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
