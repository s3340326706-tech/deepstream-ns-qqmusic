#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
probe.py —— 音源配置调试台

用法：
  python probe.py validate  <source.json>
  python probe.py search    <source.json> "关键词" [--page 1]
  python probe.py resolve   <source.json> --mid <songmid> [--quality flac]
  python probe.py lyric     <source.json> --mid <songmid>
  python probe.py flow      <source.json> "关键词" [--limit 3] [--bytes]
  python probe.py raw       <source.json> --op stream --mid <songmid>

常用开关：
  --account <path>   账号凭据 JSON（默认 ../src-private/account.json）
  --cookie "a=b;c=d" 直接贴 cookie 字符串
  --tree             把响应拍平成 `路径 = 值`，用来找字段
  --dump <file>      把原始响应存盘
  -v/--verbose       打印请求与响应摘要（cookie 自动打码）
  --bytes            拿到 URL 后取前 4KB，确认真的能下到音频
  --insecure         跳过 TLS 校验（仅调试用）
  --no-proxy         忽略系统代理
"""

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import source_engine as E

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


def _account_default():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), "src-private", "account.json")


def _print_tree(resp, limit=400):
    if not isinstance(resp, (dict, list)):
        print("    (响应不是 JSON，原文前 500 字)")
        print("    %s" % str(resp)[:500])
        return
    rows = E.flatten(resp)
    for path, val in rows[:limit]:
        print("    %-60s %s" % (path, val))
    if len(rows) > limit:
        print("    ... 还有 %d 行，用 --dump 存盘后自己看" % (len(rows) - limit))


def _probe_audio(url, verbose=False, no_proxy=False, insecure=False):
    """区间请求前 4KB，确认这个 URL 真能拿到字节。CDN 会校验 Referer。"""
    req = urllib.request.Request(url, headers={
        "Range": "bytes=0-4095",
        "User-Agent": E.DEFAULT_UA,
        "Referer": "https://y.qq.com/",
        "Origin": "https://y.qq.com",
    })
    handlers = [urllib.request.HTTPSHandler(context=E.ssl.create_default_context())]
    if insecure:
        ctx = E.ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = E.ssl.CERT_NONE
        handlers = [urllib.request.HTTPSHandler(context=ctx)]
    if no_proxy:
        handlers.append(urllib.request.ProxyHandler({}))
    opener = urllib.request.build_opener(*handlers)
    try:
        with opener.open(req, timeout=E.DEFAULT_TIMEOUT) as r:
            chunk = r.read(4096)
            ctype = r.headers.get("Content-Type", "?")
            clen = r.headers.get("Content-Range") or r.headers.get("Content-Length") or "?"
            magic = chunk[:4]
            kind = {b"fLaC": "FLAC", b"ID3": "MP3(ID3)", b"\xff\xfb": "MP3",
                    b"\xff\xf3": "MP3", b"OggS": "OGG", b"ftyp": "M4A"}.get(magic, "未知")
            print("    字节探测: status=%s type=%s len=%s magic=%s -> %s"
                  % (r.status, ctype, clen, magic, kind))
            return True
    except Exception as e:
        print("    字节探测失败: %s: %s" % (type(e).__name__, e))
        return False


def main():
    ap = argparse.ArgumentParser(description="音源配置调试台")
    ap.add_argument("cmd", choices=["validate", "search", "resolve", "lyric", "flow", "raw"])
    ap.add_argument("source")
    ap.add_argument("keyword", nargs="?", default=None)
    ap.add_argument("--mid", default=None)
    ap.add_argument("--media-mid", default=None,
                    help="QQ 取流文件名要用 media_mid；填错也能拿到 purl，但 CDN 会 404")
    ap.add_argument("--page", type=int, default=1)
    ap.add_argument("--limit", type=int, default=3)
    ap.add_argument("--quality", default=None)
    ap.add_argument("--op", default="stream", choices=["search", "stream", "lyric"])
    ap.add_argument("--account", default=None)
    ap.add_argument("--cookie", default=None)
    ap.add_argument("--tree", action="store_true")
    ap.add_argument("--dump", default=None)
    ap.add_argument("--bytes", action="store_true")
    ap.add_argument("--insecure", action="store_true")
    ap.add_argument("--no-proxy", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    try:
        src = E.load_source(a.source)
    except E.SourceError as e:
        print("[配置错误] %s" % e)
        return 2

    if a.cmd == "validate":
        auth = src.get("auth") or {}
        print("配置校验通过")
        print("  id          : %s" % src["id"])
        print("  name        : %s" % src["name"])
        print("  api_version : %s" % src["api_version"])
        print("  能力        : %s" % ", ".join(k for k in ("search", "stream", "lyric") if k in src))
        print("  登录方式    : %s" % (auth.get("type") or "none"))
        print("  白名单主机  : %d 个" % len(auth.get("allowed_hosts") or []))
        for h in auth.get("allowed_hosts") or []:
            print("      - %s" % h)
        if (src.get("stream") or {}).get("attempts"):
            print("  降级链      : %s" % " -> ".join(
                "%s(%s)" % (x.get("quality"), x.get("format")) for x in src["stream"]["attempts"]))
        print("  加密标记    : %s" % ", ".join(E.encrypted_markers(src)))
        return 0

    acct = a.account or _account_default()
    cookies = E.load_account(acct if os.path.isfile(acct) else None, a.cookie)
    if cookies:
        print("[凭据] 已载入 %d 个字段: %s" % (len(cookies), ", ".join(cookies)))
    elif (src.get("auth") or {}).get("cookie_keys"):
        print("[凭据] 未载入。请填 %s 或传 --cookie" % acct)

    ctx = E.make_ctx(src, cookies)
    sink = {}
    kw = dict(verbose=a.verbose, insecure=a.insecure, no_proxy=a.no_proxy)

    def _dump_and_tree():
        if a.dump and "resp" in sink:
            with open(a.dump, "w", encoding="utf-8") as f:
                json.dump(sink["resp"], f, ensure_ascii=False, indent=2)
            print("    已存盘: %s" % a.dump)
        if a.tree:
            _print_tree(sink.get("resp"))

    try:
        if a.cmd == "search":
            if not a.keyword:
                print("缺少关键词")
                return 2
            tracks = E.do_search(src, ctx, a.keyword, a.page, sink=sink, **kw)
            print("命中 %d 条" % len(tracks))
            for t in tracks[:a.limit]:
                print("  [%s] %s — %s (%s)  %sms"
                      % (t.get("id"), t.get("title"), t.get("artist"),
                         t.get("album"), t.get("duration_ms")))
            _dump_and_tree()
            return 0

        if a.cmd == "resolve":
            if not a.mid:
                print("缺少 --mid")
                return 2
            track = {"id": a.mid}
            if a.media_mid:
                track["extra"] = {"media_mid": a.media_mid}
            res, diag = E.do_resolve(src, ctx, track, a.quality, sink=sink, **kw)
            for q, why in diag:
                print("  [%s] %s" % (q, why))
            print(json.dumps(res, ensure_ascii=False, indent=2))
            if res.get("ok") and a.bytes:
                _probe_audio(res["url"], a.verbose, a.no_proxy, a.insecure)
            _dump_and_tree()
            return 0 if res.get("ok") else 1

        if a.cmd == "lyric":
            if not a.mid:
                print("缺少 --mid")
                return 2
            text = E.do_lyric(src, ctx, {"id": a.mid}, sink=sink, **kw)
            print("歌词 %d 字符：" % len(text))
            print(text[:800])
            _dump_and_tree()
            return 0

        if a.cmd == "flow":
            if not a.keyword:
                print("缺少关键词")
                return 2
            tracks = E.do_search(src, ctx, a.keyword, a.page, sink=sink, **kw)
            print("搜索命中 %d 条，取前 %d 条试解流" % (len(tracks), a.limit))
            ok = 0
            for t in tracks[:a.limit]:
                res, diag = E.do_resolve(src, ctx, t, a.quality, sink=sink, **kw)
                mark = "OK " if res.get("ok") else "FAIL"
                print("  %s [%s] %s — %s" % (mark, t.get("id"), t.get("title"), t.get("artist")))
                for q, why in diag:
                    print("        [%s] %s" % (q, why))
                if res.get("ok"):
                    ok += 1
                    print("        -> %s (%s)" % (res["url"][:110], res.get("format")))
                    if a.bytes:
                        _probe_audio(res["url"], a.verbose, a.no_proxy, a.insecure)
            print("成功 %d / %d" % (ok, min(len(tracks), a.limit)))
            _dump_and_tree()
            return 0 if ok else 1

        if a.cmd == "raw":
            spec = src.get({"search": "search", "stream": "stream", "lyric": "lyric"}[a.op])
            if not spec:
                print("该音源未实现 %s" % a.op)
                return 2
            if a.op == "search":
                ctx = ctx.child(args=dict(ctx.args, keyword=a.keyword or "", page=1))
                req = spec["request"]
            elif a.op == "stream":
                at = (spec.get("attempts") or [{}])[0]
                ctx = ctx.child(track={"id": a.mid or ""},
                                args=dict(ctx.args, quality=at.get("quality")))
                req = at["request"]
            else:
                ctx = ctx.child(track={"id": a.mid or ""})
                req = spec["request"]
            sink["resp"] = E.do_request(req, ctx, **kw)
            _dump_and_tree()
            return 0

    except (E.SourceError, E.RequestError, KeyError) as e:
        print("[失败] %s: %s" % (type(e).__name__, e))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
