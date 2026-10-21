#!/usr/bin/env python3
"""probe_stream.py —— 排查 QQ 音乐取流为什么 404

思路：不复用 C++ 引擎，直接在 PC 上按 qqmusic.json 里同样的请求形态走一遍：
  搜索(带 media_mid) → UrlGetVkey 取 purl → 真发一次 Range 下载看 HTTP 状态。

核心对比：同一首歌，filename 用 songmid 还是 media_mid。
QQ 的音频文件名用的是 media_mid，歌单接口不返回它，我们的代码会回退 songmid；
如果两者不相同，vkey 会返回一个"语法正确但文件不存在"的 purl → 下载 404。

用法：
  python app/tools/probe_stream.py "晴天"
  python app/tools/probe_stream.py "晴天" --cookie-file dist/switch/se_sources/account.json
"""
import argparse
import json
import re
import sys
import urllib.parse
import urllib.request

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36")
HDR_BASE = {
    "Referer": "https://y.qq.com/",
    "Origin": "https://y.qq.com",
    "User-Agent": UA,
}


def parse_cookie(s):
    out = {}
    for part in s.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        k, v = part.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def gtk(skey):
    """QQ 的 g_tk：djb2，种子 5381。注意不是扫码用的 hash33。"""
    h = 5381
    for ch in skey:
        h = (h * 33 + ord(ch)) & 0x7FFFFFFF
    return h


def digits(s):
    return "".join(c for c in s if c.isdigit())


def http(url, data=None, headers=None, timeout=15):
    h = dict(HDR_BASE)
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, data=data, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, b""
    except Exception as e:                      # 网络类错误
        return -1, str(e).encode()


def search(keyword, cookie, n=10):
    """和 qqmusic.json 的 search.request 保持一致（client_search_cp + new_json）"""
    qs = urllib.parse.urlencode({
        "w": keyword, "format": "json", "n": n, "p": 1,
        "cr": 1, "t": 0, "aggr": 1, "lossless": 0, "new_json": 1,
    })
    st, body = http("https://c.y.qq.com/soso/fcgi-bin/client_search_cp?" + qs,
                    headers={"Cookie": cookie} if cookie else None)
    if st != 200:
        print("搜索失败 http=%s" % st, file=sys.stderr)
        return []
    j = json.loads(body.decode("utf-8", "replace"))
    return (j.get("data") or {}).get("song", {}).get("list") or []


def vkey(songmid, filename, cookie):
    """按 qqmusic.json 的 stream.attempts 形态请求 purl"""
    ck = parse_cookie(cookie) if cookie else {}
    skey = (ck.get("qqmusic_key") or ck.get("qm_keyst") or ck.get("music_key")
            or ck.get("p_skey") or ck.get("skey") or "")
    uin = digits(ck.get("uin", ""))
    guid = ck.get("pgv_pvid") or ck.get("qqmusic_guid") or ck.get("guid") or "0"

    body = {
        "comm": {
            "ct": 24, "cv": 0, "uin": uin, "format": "json",
            "g_tk": gtk(skey), "g_tk_new_20200303": gtk(skey),
        },
        "req": {
            "module": "music.vkey.GetVkey", "method": "UrlGetVkey",
            "param": {
                "guid": guid, "songmid": [songmid], "songtype": [0],
                "uin": uin, "loginflag": 1, "platform": "20",
                "filename": [filename],
            },
        },
    }
    hdr = {"Content-Type": "application/json"}
    if cookie:
        hdr["Cookie"] = cookie
    st, raw = http("https://u.y.qq.com/cgi-bin/musicu.fcg",
                   data=json.dumps(body).encode(), headers=hdr)
    if st != 200:
        return None, "http=%s" % st
    j = json.loads(raw.decode("utf-8", "replace"))
    req = j.get("req") or {}
    data = req.get("data") or {}
    info = (data.get("midurlinfo") or [{}])[0]
    purl = info.get("purl") or ""
    if not purl:
        return None, "code=%s result=%s purl空" % (req.get("code"), info.get("result"))
    if not purl.startswith("http"):
        sip = (data.get("sip") or ["https://ws.stream.qqmusic.qq.com/"])[0]
        if not sip.endswith("/"):
            sip += "/"
        purl = sip + purl.lstrip("/")
    return purl, ""


def probe(url):
    """真发一次请求，只取前 2KB。200/206 = 文件存在；404 = 路径不存在。"""
    st, body = http(url, headers={
        "Range": "bytes=0-2047",
        "Referer": "https://y.qq.com/",
        "User-Agent": UA,
    })
    head = body[:4]
    return st, head


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keyword")
    ap.add_argument("--cookie-file", default="dist/switch/se_sources/account.json")
    ap.add_argument("--num", type=int, default=10)
    args = ap.parse_args()

    cookie = ""
    try:
        with open(args.cookie_file, encoding="utf-8") as f:
            cookie = json.load(f).get("cookie", "")
    except Exception:
        print("（没读到 cookie，按未登录测试）")

    songs = search(args.keyword, cookie, args.num)
    if not songs:
        print("没有搜索结果")
        return

    print("%-26s %-16s %-16s %-8s %-8s" % ("歌名", "songmid", "media_mid", "是否相同", "状态"))
    print("-" * 92)

    diff = 0
    total = 0
    for s in songs:
        title = (s.get("title") or s.get("name") or "")[:24]
        mid = s.get("mid") or s.get("songmid") or ""
        media_mid = ((s.get("file") or {}).get("media_mid") or "")
        same = (media_mid == mid) or (media_mid == "")
        if not same:
            diff += 1
        total += 1

        rows = []
        for label, fname in (
            ("M500(songmid)", "M500%s.mp3" % mid),
            ("M500(media_mid)", "M500%s.mp3" % (media_mid or mid)),
        ):
            purl, err = vkey(mid, fname, cookie)
            if not purl:
                rows.append("%s -> %s" % (label, err))
                continue
            st, head = probe(purl)
            rows.append("%s -> HTTP %s %s" % (label, st, head[:4]))

        print("%-26s %-16s %-16s %-8s" % (title, mid, media_mid or "(无)", "同" if same else "不同"))
        for r in rows:
            print("    " + r)

    print("-" * 92)
    print("共 %d 首，其中 media_mid 与 songmid 不同（或缺失）%d 首" % (total, diff))


if __name__ == "__main__":
    main()
