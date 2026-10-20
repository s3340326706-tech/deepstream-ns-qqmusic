#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
source_engine.py —— 音源配置引擎参考实现 v1

用途：
  1. 验证《音源配置引擎规范 v1》的 DSL 是否足够表达真实接口
  2. 在实际接口上跑通 search / stream / lyric，为 C++ 移植提供语义基准
  3. 校准 source JSON 里的字段路径（配合 --tree / --dump）

依赖：仅 Python 标准库（urllib），无第三方包。
      C++ 版移植时对应 libcurl + nlohmann/json。

安全模型（与规范一致）：
  - host 白名单：配置里任何请求的 host 不在 auth.allowed_hosts 内 -> 整个配置拒绝加载
  - cookie 作用域：$cookie.* 只对白名单 host 生效
  - 无代码执行：函数白名单，配置是数据不是脚本
"""

import base64
import hashlib
import json
import os
import random
import re
import ssl
import string
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

SUPPORTED_API_VERSION = 1
DEFAULT_TIMEOUT = 8
MAX_BODY = 4 * 1024 * 1024
MAX_LIST = 200
MAX_ATTEMPTS = 8
MAX_FILE = 256 * 1024

# 加密流标记：配置只能追加，不能清空（fail-closed）
BASE_ENCRYPTED = (".mflac", ".mgg", ".mflac0", ".mgg1", ".mmp4")

DEFAULT_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")


class SourceError(Exception):
    """配置错误 / 加载失败"""


class RequestError(Exception):
    """请求执行失败"""


# --------------------------------------------------------------------------
# 配置加载与校验
# --------------------------------------------------------------------------

def iter_requests(src):
    """遍历配置里所有 request 对象，用于 host 白名单校验。"""
    if isinstance(src.get("search"), dict) and "request" in src["search"]:
        yield "search.request", src["search"]["request"]

    stream = src.get("stream") or {}
    for i, at in enumerate(stream.get("attempts") or []):
        if isinstance(at, dict) and "request" in at:
            yield "stream.attempts[%d].request" % i, at["request"]

    if isinstance(src.get("lyric"), dict) and "request" in src["lyric"]:
        yield "lyric.request", src["lyric"]["request"]

    auth = src.get("auth") or {}
    for i, r in enumerate(auth.get("post_login") or []):
        yield "auth.post_login[%d]" % i, r


def _host_of(url):
    return (urllib.parse.urlsplit(url).hostname or "").lower()


def load_source(path):
    """读入 + 校验。任何一条不过就抛 SourceError，绝不静默降级。"""
    if not os.path.isfile(path):
        raise SourceError("文件不存在: %s" % path)
    if os.path.getsize(path) > MAX_FILE:
        raise SourceError("配置文件超过 %d 字节上限" % MAX_FILE)

    with open(path, "r", encoding="utf-8") as f:
        try:
            src = json.load(f)
        except json.JSONDecodeError as e:
            raise SourceError("JSON 语法错误: %s (行 %d 列 %d)" % (e.msg, e.lineno, e.colno))

    if not isinstance(src, dict):
        raise SourceError("顶层必须是对象")

    ver = src.get("api_version")
    if not isinstance(ver, int):
        raise SourceError("缺少 api_version")
    if ver > SUPPORTED_API_VERSION:
        raise SourceError("api_version=%d 高于引擎支持的 %d" % (ver, SUPPORTED_API_VERSION))

    for key in ("id", "name"):
        if not src.get(key):
            raise SourceError("缺少必填字段 %s" % key)

    if not any(k in src for k in ("search", "stream", "lyric")):
        raise SourceError("search / stream / lyric 至少要有一个")

    stream = src.get("stream") or {}
    attempts = stream.get("attempts") or []
    if len(attempts) > MAX_ATTEMPTS:
        raise SourceError("stream.attempts 最多 %d 档，当前 %d" % (MAX_ATTEMPTS, len(attempts)))

    auth = src.get("auth") or {}
    allowed = [h.lower() for h in (auth.get("allowed_hosts") or [])]
    cookie_keys = auth.get("cookie_keys") or []
    cookie_fmt = auth.get("cookie_format") or "keys"

    if (cookie_keys or cookie_fmt == "raw") and not allowed:
        raise SourceError("声明了凭据就必须给 allowed_hosts（否则凭据可被发往任意主机）")

    # 所有请求的 host 必须在白名单内
    for label, req in iter_requests(src):
        url = req.get("url")
        if not isinstance(url, str) or not url.startswith(("http://", "https://")):
            raise SourceError("%s.url 不是合法 http(s) 地址" % label)
        host = _host_of(url)
        if allowed and host not in allowed:
            raise SourceError("%s 的主机 %s 不在 allowed_hosts 内" % (label, host))

    return src


def encrypted_markers(src):
    """默认标记 + 配置追加。配置无法移除默认值。"""
    extra = ((src.get("stream") or {}).get("encrypted_markers")) or []
    out = list(BASE_ENCRYPTED)
    for m in extra:
        if isinstance(m, str) and m.lower() not in (x.lower() for x in out):
            out.append(m.lower())
    return out


# --------------------------------------------------------------------------
# 表达式求值
# --------------------------------------------------------------------------

_INDEX_RE = re.compile(r"\[(\d+)\]")


def _walk(obj, path):
    """'req_0.data.midurlinfo[0].purl' -> 取值；缺键返回 None。"""
    cur = obj
    for seg in path.split("."):
        if not seg:
            continue
        name = seg.split("[")[0]
        if name:
            if not isinstance(cur, dict) or name not in cur:
                return None
            cur = cur[name]
        for idx in _INDEX_RE.findall(seg):
            if not isinstance(cur, list):
                return None
            i = int(idx)
            if i >= len(cur):
                return None
            cur = cur[i]
    return cur


def _digits(s):
    return re.sub(r"\D", "", str(s or ""))


def call_fn(name, args):
    """函数白名单。配置无法新增函数。"""
    if name == "hash33":
        h = 0
        for ch in str(args[0] if args else ""):
            h += (h << 5) + ord(ch)
            h &= 0xFFFFFFFF
        return h & 0x7FFFFFFF
    if name == "gtk":
        # QQ 生态标准 g_tk：种子 5381（djb2），与扫码用的 hash33(种子 0) 不同
        h = 5381
        for ch in str(args[0] if args else ""):
            h += (h << 5) + ord(ch)
            h &= 0xFFFFFFFF
        return h & 0x7FFFFFFF
    if name == "md5":
        return hashlib.md5(str(args[0]).encode("utf-8")).hexdigest()
    if name == "sha1":
        return hashlib.sha1(str(args[0]).encode("utf-8")).hexdigest()
    if name == "base64":
        return base64.b64encode(str(args[0]).encode("utf-8")).decode("ascii")
    if name == "urlencode":
        return urllib.parse.quote(str(args[0]), safe="")
    if name == "concat":
        return "".join("" if a is None else str(a) for a in args)
    if name == "substr":
        s = str(args[0] or "")
        a = int(args[1]) if len(args) > 1 else 0
        b = int(args[2]) if len(args) > 2 else len(s)
        return s[a:b]
    if name == "default":
        v = args[0] if args else None
        fb = args[1] if len(args) > 1 else ""
        return fb if v in (None, "", [], {}) else v
    if name == "int":
        try:
            return int(str(args[0]).strip())
        except (TypeError, ValueError):
            d = _digits(args[0] if args else "")
            return int(d) if d else 0
    if name == "mul":
        try:
            out = 1
            for x in args:
                out *= float(x)
            return int(out) if float(out).is_integer() else out
        except (TypeError, ValueError):
            return 0
    if name == "digits":
        return _digits(args[0] if args else "")
    if name == "json":
        try:
            return json.loads(args[0])
        except Exception:
            return None
    raise SourceError("未知函数 %s（不在白名单内）" % name)


class Ctx(object):
    """求值上下文"""

    def __init__(self, src, cookies=None):
        self.src = src
        self.cookies = cookies or {}
        self.args = {}
        self.track = {}
        self.item = None
        self.resp = None
        self.host = ""
        self.cookie_allowed = True
        self._var_cache = {}
        self._var_stack = []

    def child(self, **kw):
        c = Ctx.__new__(Ctx)
        c.__dict__.update({
            "src": self.src, "cookies": self.cookies, "args": self.args,
            "track": self.track, "item": self.item, "resp": self.resp,
            "host": self.host, "cookie_allowed": self.cookie_allowed,
            "_var_cache": self._var_cache, "_var_stack": self._var_stack,
        })
        c.__dict__.update(kw)
        return c


def lookup(expr, ctx):
    """$arg.x / $cookie.x / $var.x / $track.x / $item.x / $resp.* / $now / $rand16"""
    if expr == "$now":
        return int(time.time() * 1000)
    if expr == "$rand16":
        return "".join(random.choice(string.hexdigits.lower()) for _ in range(16))
    if expr.startswith("$now."):
        return int(time.time() * 1000)

    head, _, rest = expr[1:].partition(".")
    if head == "arg":
        return _walk(ctx.args, rest)
    if head == "cookie":
        if not ctx.cookie_allowed:
            raise SourceError(
                "拒绝把 cookie 发往 %s（不在 allowed_hosts 内）" % (ctx.host or "?"))
        return _walk(ctx.cookies, rest)
    if head == "var":
        return resolve_var(rest, ctx)
    if head == "track":
        return _walk(ctx.track, rest)
    if head == "item":
        return _walk(ctx.item, rest)
    if head == "resp":
        return _walk(ctx.resp, rest)
    raise SourceError("未知表达式命名空间: %s" % expr)


def resolve_var(name, ctx):
    if name in ctx._var_cache:
        return ctx._var_cache[name]
    if name in ctx._var_stack:
        raise SourceError("vars 循环引用: %s" % " -> ".join(ctx._var_stack + [name]))
    defs = ctx.src.get("vars") or {}
    if name not in defs:
        return None
    ctx._var_stack.append(name)
    try:
        val = resolve(defs[name], ctx)
    finally:
        ctx._var_stack.pop()
    ctx._var_cache[name] = val
    return val


def resolve(node, ctx):
    if isinstance(node, dict):
        if "fn" in node:
            args = [resolve(a, ctx) for a in (node.get("args") or [])]
            return call_fn(node["fn"], args)
        return {k: resolve(v, ctx) for k, v in node.items() if not k.startswith("_")}
    if isinstance(node, list):
        return [resolve(x, ctx) for x in node]
    if isinstance(node, str) and node.startswith("$"):
        return lookup(node, ctx)
    return node


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

def _maybe_jsonp(text):
    """去掉 callback(...) 包裹。QQ 系接口经常这样返回。"""
    s = text.strip()
    m = re.match(r"^[\w$.]*\s*\((.*)\)\s*;?\s*$", s, re.S)
    if m:
        try:
            return json.loads(m.group(1))
        except Exception:
            return None
    return None


def parse_body(raw, ctype=""):
    text = raw.decode("utf-8", "replace")
    try:
        return json.loads(text)
    except Exception:
        pass
    jp = _maybe_jsonp(text)
    if jp is not None:
        return jp
    return text


def build_url(req, ctx):
    url = resolve(req["url"], ctx)
    params = req.get("params") or {}
    if params:
        pairs = []
        for k, v in params.items():
            if k.startswith("_"):
                continue
            val = resolve(v, ctx)
            if val is None:
                continue
            pairs.append((k, "" if val is False else str(val)))
        if pairs:
            sep = "&" if ("?" in url) else "?"
            url = url + sep + urllib.parse.urlencode(pairs)
    return url


def do_request(req, ctx, verbose=False, insecure=False, no_proxy=False):
    if not isinstance(req, dict):
        raise RequestError("request 结构缺失")

    url = build_url(req, ctx)
    host = _host_of(url)

    allowed = [h.lower() for h in ((ctx.src.get("auth") or {}).get("allowed_hosts") or [])]
    if allowed and host not in allowed:
        raise SourceError("请求主机 %s 不在 allowed_hosts 内" % host)

    ctx.host = host
    ctx.cookie_allowed = (not allowed) or (host in allowed)

    headers = {}
    for k, v in (req.get("headers") or {}).items():
        if k.startswith("_"):
            continue
        val = resolve(v, ctx)
        if val is not None:
            headers[k] = str(val)
    headers.setdefault("User-Agent", DEFAULT_UA)

    cookie_keys = (ctx.src.get("auth") or {}).get("cookie_keys") or []
    cookie_fmt = (ctx.src.get("auth") or {}).get("cookie_format") or "keys"
    cookies = {}
    if ctx.cookie_allowed:
        if cookie_fmt == "raw":
            # 整串导入模式：把用户粘贴的整份 Cookie 原样带过去
            cookies = {k: v for k, v in ctx.cookies.items() if v}
        elif cookie_keys:
            for k in cookie_keys:
                if ctx.cookies.get(k):
                    cookies[k] = str(ctx.cookies[k])
    if cookies:
        headers["Cookie"] = "; ".join("%s=%s" % (k, v) for k, v in cookies.items())

    data = None
    if "body_json" in req:
        body = resolve(req["body_json"], ctx)
        data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers.setdefault("Content-Type", "application/json")
    elif "body_form" in req:
        body = resolve(req["body_form"], ctx) or {}
        data = urllib.parse.urlencode(
            [(k, v) for k, v in body.items() if not k.startswith("_")]).encode("utf-8")
        headers.setdefault("Content-Type", "application/x-www-form-urlencoded")
    elif req.get("body"):
        data = str(resolve(req["body"], ctx)).encode("utf-8")

    method = (req.get("method") or ("POST" if data else "GET")).upper()

    if verbose:
        print("  > %s %s" % (method, url))
        for k in ("Content-Type", "Referer"):
            if k in headers:
                print("  > %s: %s" % (k, headers[k]))
        if cookies:
            print("  > Cookie: %s" % ", ".join("%s=***" % k for k in cookies))
        if data and len(data) < 2000:
            print("  > body: %s" % data.decode("utf-8", "replace"))

    ssl_ctx = ssl.create_default_context()
    if insecure:
        ssl_ctx.check_hostname = False
        ssl_ctx.verify_mode = ssl.CERT_NONE

    handlers = [urllib.request.HTTPSHandler(context=ssl_ctx)]
    if no_proxy:
        handlers.append(urllib.request.ProxyHandler({}))
    opener = urllib.request.build_opener(*handlers)

    request = urllib.request.Request(url, data=data, method=method)
    for k, v in headers.items():
        request.add_header(k, v)

    t0 = time.time()
    try:
        with opener.open(request, timeout=DEFAULT_TIMEOUT) as resp:
            raw = resp.read(MAX_BODY + 1)
            if len(raw) > MAX_BODY:
                raise RequestError("响应超过 %d 字节上限" % MAX_BODY)
            ctype = resp.headers.get("Content-Type", "")
            status = resp.status
    except urllib.error.HTTPError as e:
        return {"_error": "HTTP %d" % e.code, "_status": e.code,
                "_elapsed_ms": int((time.time() - t0) * 1000)}
    except Exception as e:
        raise RequestError("%s: %s" % (type(e).__name__, e))

    parsed = parse_body(raw, ctype)
    if verbose:
        preview = raw[:1200].decode("utf-8", "replace")
        print("  < %d  %dms  %d bytes" % (status, int((time.time() - t0) * 1000), len(raw)))
        print("  < %s" % preview.replace("\n", " ")[:1000])

    if isinstance(parsed, dict):
        parsed["_status"] = status
        parsed["_elapsed_ms"] = int((time.time() - t0) * 1000)
    return parsed


# --------------------------------------------------------------------------
# 三个操作
# --------------------------------------------------------------------------

def do_search(src, ctx, keyword, page=1, verbose=False, insecure=False, no_proxy=False,
              sink=None):
    spec = src.get("search")
    if not spec:
        raise SourceError("该音源未实现 search")

    ctx = ctx.child(args=dict(ctx.args, keyword=keyword, page=page))
    resp = do_request(spec["request"], ctx, verbose, insecure, no_proxy)
    ctx.resp = resp
    if sink is not None:
        sink["resp"] = resp

    listing = resolve(spec.get("list", "$resp"), ctx)
    if not isinstance(listing, list):
        raise RequestError("search.list 求值结果不是数组（实际是 %s）" % type(listing).__name__)

    out = []
    for elem in listing[:MAX_LIST]:
        item_ctx = ctx.child(item=elem)
        track = {}
        for k, v in (spec.get("map") or {}).items():
            if k.startswith("_"):
                continue
            track[k] = resolve(v, item_ctx)
        out.append(track)
    return out


def do_resolve(src, ctx, track, quality_pref=None, verbose=False,
               insecure=False, no_proxy=False, sink=None):
    """按 attempts 顺序降级，返回 (StreamResult, 诊断列表)"""
    stream = src.get("stream")
    if not stream:
        raise SourceError("该音源未实现 stream")

    empty_vals = stream.get("empty_values")
    if empty_vals is None:
        empty_vals = ["", None]
    markers = encrypted_markers(src)
    diag = []

    attempts = stream.get("attempts") or []
    if quality_pref:
        head = [a for a in attempts if a.get("quality") == quality_pref]
        attempts = head + [a for a in attempts if a.get("quality") != quality_pref]

    for at in attempts:
        quality = at.get("quality", "?")
        ctx2 = ctx.child(track=track, args=dict(ctx.args, quality=quality))
        try:
            resp = do_request(at["request"], ctx2, verbose, insecure, no_proxy)
            ctx2.resp = resp
            if sink is not None:
                sink["resp"] = resp
                sink.setdefault("all", []).append((quality, resp))
            url = resolve(at.get("url"), ctx2)
        except (RequestError, SourceError) as e:
            diag.append((quality, "请求失败: %s" % e))
            continue

        # 接口级错误码：能区分「没权限」和「字段路径写错」
        err = at.get("error")
        if isinstance(err, dict) and err.get("path"):
            code = resolve(err["path"], ctx2)
            ok_vals = err.get("ok_values")
            if ok_vals is not None and code not in ok_vals:
                diag.append((quality, "接口返回 code=%s（%s）"
                             % (code, err.get("hint", "未登录 / 无权益 / 参数错误"))))
                continue

        if url in empty_vals or not url:
            diag.append((quality, "purl 为空（无此音质权益 / 风控 / 字段路径不对）"))
            continue

        # 相对地址拼 CDN 主机头（QQ 的 purl 常常是相对路径，要配 sip[0]）
        base = at.get("url_base")
        if base and not str(url).lower().startswith("http"):
            baseval = resolve(base, ctx2) or ""
            if baseval:
                if not baseval.endswith("/"):
                    baseval += "/"
                url = baseval + str(url).lstrip("/")

        low = str(url).lower()
        hit = [m for m in markers if m in low]
        if hit:
            # fail-closed：识别到加密流就放弃，绝不解密
            diag.append((quality, "命中加密标记 %s，按策略放弃" % hit[0]))
            return ({"ok": False, "error": "该曲目仅提供加密流，按策略不播放（不做解密）",
                     "quality": quality}, diag)

        exp = at.get("expires_in")
        out = {"ok": True, "url": url, "format": at.get("format", ""), "quality": quality}
        if isinstance(exp, (int, float)) and exp > 0:
            out["expires_at"] = int(time.time() * 1000) + int(exp * 1000)
        return (out, diag)

    return ({"ok": False, "error": "该曲目当前账号无可播流",
             "detail": "; ".join("%s: %s" % d for d in diag)}, diag)


def do_lyric(src, ctx, track, verbose=False, insecure=False, no_proxy=False, sink=None):
    spec = src.get("lyric")
    if not spec:
        raise SourceError("该音源未实现 lyric")

    ctx = ctx.child(track=track)
    resp = do_request(spec["request"], ctx, verbose, insecure, no_proxy)
    ctx.resp = resp
    if sink is not None:
        sink["resp"] = resp

    text = resolve(spec.get("text", "$resp"), ctx)
    enc = (spec.get("encoding") or "plain").lower()
    if not isinstance(text, str):
        return ""

    if enc == "base64":
        try:
            return base64.b64decode(text).decode("utf-8", "replace")
        except Exception:
            return ""
    if enc == "html":
        # QQ 歌词接口 nobase64=1 时仍是 HTML 实体（&#10; / &apos; 等）
        import html as _html
        return _html.unescape(text)
    if enc == "json_escaped":
        try:
            return json.loads('"%s"' % text)
        except Exception:
            return text
    return text


# --------------------------------------------------------------------------
# 辅助：把响应拍平成路径表，用于校准字段
# --------------------------------------------------------------------------

def flatten(obj, prefix="", depth=0, max_depth=6):
    lines = []
    if depth > max_depth:
        lines.append((prefix, "..."))
        return lines
    if isinstance(obj, dict):
        for k, v in obj.items():
            p = "%s.%s" % (prefix, k) if prefix else k
            lines.extend(flatten(v, p, depth + 1, max_depth))
    elif isinstance(obj, list):
        for i, v in enumerate(obj[:3]):
            p = "%s[%d]" % (prefix, i)
            lines.extend(flatten(v, p, depth + 1, max_depth))
        if len(obj) > 3:
            lines.append(("  ... (%d items)" % len(obj), ""))
    else:
        s = repr(obj)
        if len(s) > 120:
            s = s[:117] + "..."
        lines.append((prefix, s))
    return lines


def load_account(path=None, cookie_str=None):
    """支持两种存储形态：
       1) 结构化 {uin, skey, ...}
       2) 整串 {cookie: "a=b; c=d; ..."}  ← Switch 侧 accounts.ini 就是这种
    """
    cookies = {}
    if path and os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
        for k, v in raw.items():
            if k.startswith("_") or not isinstance(v, str):
                continue
            if k == "cookie":
                cookies.update(parse_cookie_string(v))
            else:
                cookies[k] = v
    if cookie_str:
        cookies.update(parse_cookie_string(cookie_str))
    return cookies


def parse_cookie_string(s):
    out = {}
    for part in str(s or "").replace("\n", ";").split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        k, v = part.split("=", 1)
        k = k.strip()
        if k:
            out[k] = v.strip()
    return out


def cookie_first(cookies, *names):
    """QQ 字段名不统一，按候选顺序取第一个非空值。"""
    for n in names:
        v = cookies.get(n)
        if v:
            return v
    return ""


def make_ctx(src, cookies):
    return Ctx(src, cookies)


if __name__ == "__main__":
    print("这是库文件，命令行入口请用 probe.py")
    sys.exit(0)
