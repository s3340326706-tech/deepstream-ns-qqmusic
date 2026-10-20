# -*- coding: utf-8 -*-
"""一次性维护脚本：把面向控制台的日志/异常文案改成 ASCII 英文。

原因：libnx 的 console 只有 8x8 ASCII 位图字体，CJK 会变成乱码并且因为每字
3 列而撑爆行宽，导致整屏错行。异常/诊断文本用英文是更通用的做法（要开源），
中文留给正式 GUI 的 i18n 表。
"""
import io
import re
import sys

ROOT = r"C:/Users/admin/nxhomebrew/engine_cpp"

# (文件, 旧串, 新串, 期望出现次数)
RULES = [
    # ---------- tools/probe.cpp ----------
    ("tools/probe.cpp", 'printf("\\n%d 项失败\\n", fail);', 'printf("\\n%d failed\\n", fail);', 1),
    ("tools/probe.cpp", '"    字节探测失败: %s\\n"', '"    probe failed: %s\\n"', 1),
    ("tools/probe.cpp", 'const char* kind = "未知";', 'const char* kind = "unknown";', 1),
    ("tools/probe.cpp", '"    字节探测: status=%ld', '"    probe: status=%ld', 1),
    ("tools/probe.cpp", '"[网络] %s (%s, 信号%d, 等待%ds) ip=%s dns=%s\\n"',
     '"[net] %s (%s, signal %d, waited %ds) ip=%s dns=%s\\n"', 1),
    ("tools/probe.cpp", '? "已连接" : "未连接"', '? "connected" : "DOWN"', 1),
    ("tools/probe.cpp", '? "WiFi" : "有线"', '? "wifi" : "ethernet"', 1),
    ("tools/probe.cpp", 'printf("内置回归：%s\\n音源：%s\\n", a.keyword.c_str(), a.source.c_str());',
     'printf("built-in regression (fixed CJK search term)\\nsource: %s\\n", a.source.c_str());', 1),
    ("tools/probe.cpp", 'printf("\\n按 + 退出\\n");', 'printf("\\nPress + to exit\\n");', 2),
    ("tools/probe.cpp", '"用法: probe <validate|search|resolve|lyric|flow|raw|selftest> <source.json> [参数]\\n"',
     '"usage: probe <validate|search|resolve|lyric|flow|raw|selftest> <source.json> [options]\\n"', 1),
    ("tools/probe.cpp", '"[配置错误] %s\\n"', '"[config error] %s\\n"', 1),
    ("tools/probe.cpp", 'printf("配置校验通过\\n");', 'printf("source config OK\\n");', 1),
    ("tools/probe.cpp", '"  白名单主机  : %zu 个\\n"', '"  allowed_hosts : %zu\\n"', 1),
    ("tools/probe.cpp", '"  加密标记    :"', '"  encrypted_markers:"', 1),
    ("tools/probe.cpp", '"[凭据] 已载入 %zu 个字段\\n"', '"[cred] %zu fields loaded\\n"', 1),
    ("tools/probe.cpp", '"[凭据] ⚠️ 没有命中任何登录票据，多半未登录\\n"',
     '"[cred] WARN no login ticket matched - probably not logged in\\n"', 1),
    ("tools/probe.cpp", '"[凭据] ⚠️ 没有 pgv_pvid：QQ 的 purl 会恒空（它就是 guid）\\n"',
     '"[cred] WARN no pgv_pvid - purl will be empty (it IS the guid)\\n"', 1),
    ("tools/probe.cpp", 'printf("缺少关键词\\n")', 'printf("missing keyword\\n")', 2),
    ("tools/probe.cpp", 'printf("命中 %zu 条\\n", list.size());', 'printf("hits: %zu\\n", list.size());', 1),
    ("tools/probe.cpp", 'printf("缺少 --mid\\n")', 'printf("missing --mid\\n")', 2),
    ("tools/probe.cpp", '"歌词 %zu 字节：\\n%s\\n"', '"lyric %zu bytes:\\n%s\\n"', 1),
    ("tools/probe.cpp", '"搜索命中 %zu 条，取前 %d 条试解流\\n"', '"hits: %zu, resolving first %d\\n"', 1),
    ("tools/probe.cpp", 'printf("成功 %d / %d\\n", ok,', 'printf("OK %d / %d\\n", ok,', 1),
    ("tools/probe.cpp", '"[失败] %s\\n"', '"[fail] %s\\n"', 1),
    ("tools/probe.cpp", 'printf("未知命令 %s\\n", a.cmd.c_str());', 'printf("unknown command %s\\n", a.cmd.c_str());', 1),
    ("tools/probe.cpp", '"  [%s] %s — %s (%s)  %lld ms\\n"', '"  [%s] %s - %s (%s)  %lld ms\\n"', 1),
    ("tools/probe.cpp", '"  %s [%s] %s — %s\\n"', '"  %s [%s] %s - %s\\n"', 1),

    # ---------- src/source_engine.cpp ----------
    ("src/source_engine.cpp", '"响应不是 JSON（"', '"response is not JSON ("', 1),
    ("src/source_engine.cpp", '" 字节，首段："', '" bytes, head: "', 1),
    ("src/source_engine.cpp", '"；解析错误："', '"; parse error: "', 1),
    ("src/source_engine.cpp", '+ err1 + "）"', '+ err1 + ")"', 1),
    ("src/source_engine.cpp", '"vars 循环引用: "', '"vars cycle: "', 1),
    ("src/source_engine.cpp", '"未知函数 "', '"unknown function "', 1),
    ("src/source_engine.cpp", '"（不在白名单内）"', '" (not in whitelist)"', 1),
    ("src/source_engine.cpp", '"拒绝把 cookie 发往 "', '"refusing to send cookie to "', 1),
    ("src/source_engine.cpp", '"未知表达式命名空间: "', '"unknown expression namespace: "', 1),
    ("src/source_engine.cpp", '"request 结构缺失"', '"request structure missing"', 1),
    ("src/source_engine.cpp", '"请求主机 "', '"request host "', 1),
    ("src/source_engine.cpp", '" 的主机 "', '" host "', 1),
    ("src/source_engine.cpp", '" 不在 allowed_hosts 内"', '" not in allowed_hosts"', 2),
    ("src/source_engine.cpp", '"文件不存在: "', '"file not found: "', 1),
    ("src/source_engine.cpp", '"配置文件超过 256KB 上限"', '"config file exceeds 256KB cap"', 1),
    ("src/source_engine.cpp", '"JSON 语法错误: "', '"JSON syntax error: "', 1),
    ("src/source_engine.cpp", '"顶层必须是对象"', '"top level must be an object"', 1),
    ("src/source_engine.cpp", '"缺少 api_version"', '"missing api_version"', 1),
    ("src/source_engine.cpp", '"api_version 高于引擎支持的 1"', '"api_version higher than supported (1)"', 1),
    ("src/source_engine.cpp", '"缺少必填字段 id"', '"missing required field: id"', 1),
    ("src/source_engine.cpp", '"缺少必填字段 name"', '"missing required field: name"', 1),
    ("src/source_engine.cpp", '"search / stream / lyric 至少要有一个"',
     '"at least one of search / stream / lyric is required"', 1),
    ("src/source_engine.cpp", '"stream.attempts 最多 8 档"', '"stream.attempts allows at most 8 entries"', 1),
    ("src/source_engine.cpp", '"声明了凭据就必须给 allowed_hosts（否则凭据可被发往任意主机）"',
     '"credentials declared but allowed_hosts missing (cookies could go to arbitrary hosts)"', 1),
    ("src/source_engine.cpp", '".url 不是字符串"', '".url is not a string"', 1),
    ("src/source_engine.cpp", '".url 不是合法 http(s) 地址"', '".url is not a valid http(s) url"', 1),
    ("src/source_engine.cpp", '"该音源未实现 search"', '"source does not implement search"', 2),
    ("src/source_engine.cpp", '"该音源未实现 stream"', '"source does not implement stream"', 2),
    ("src/source_engine.cpp", '"该音源未实现 lyric"', '"source does not implement lyric"', 2),
    ("src/source_engine.cpp", '"search.list 求值结果不是数组；resp="',
     '"search.list did not evaluate to an array; resp="', 1),
    ("src/source_engine.cpp", '"stream.attempts 缺失"', '"stream.attempts missing"', 2),
    ("src/source_engine.cpp", '"请求失败: "', '"request failed: "', 1),
    ("src/source_engine.cpp", 'std::string hint = "见接口返回";', 'std::string hint = "see API response";', 1),
    ("src/source_engine.cpp", '"接口返回 code="', '"API returned code="', 1),
    ("src/source_engine.cpp", '"（" + hint + "）"', '" (" + hint + ")"', 1),
    ("src/source_engine.cpp", '"purl 为空（无此音质权益 / 风控 / 字段路径不对）"',
     '"purl empty (no entitlement for this quality / risk control / wrong field path)"', 1),
    ("src/source_engine.cpp", '"命中加密标记 "', '"encrypted marker hit: "', 1),
    ("src/source_engine.cpp", '"，按策略放弃"', '", skipped by policy"', 1),
    ("src/source_engine.cpp", '"该曲目仅提供加密流，按策略不播放（不做解密）"',
     '"only encrypted stream available; not played by policy (no decryption)"', 1),
    ("src/source_engine.cpp", '"该曲目当前账号无可播流"', '"no playable stream for current account"', 1),

    # ---------- src/http.cpp ----------
    ("src/http.cpp", '"响应超过上限"', '"response exceeds size cap"', 1),
]

CJK = re.compile(r"[\u3000-\u303f\u4e00-\u9fff\uff00-\uffef\u2018-\u201f\u2026\u2014\u26a0\ufe0f]")


def main():
    cache = {}
    errors = []
    for rel, old, new, want in RULES:
        if rel not in cache:
            cache[rel] = io.open(ROOT + "/" + rel, encoding="utf-8").read()
        txt = cache[rel]
        got = txt.count(old)
        if got != want:
            errors.append("%s: expected %d, found %d -> %r" % (rel, want, got, old[:50]))
            continue
        cache[rel] = txt.replace(old, new)

    if errors:
        print("ABORT, nothing written:")
        for e in errors:
            print("  " + e)
        return 1

    for rel, txt in cache.items():
        io.open(ROOT + "/" + rel, "w", encoding="utf-8", newline="").write(txt)
        print("written: " + rel)

    # 复查：面向输出的字符串里不该再有 CJK（注释允许保留）
    for rel in cache:
        left = 0
        for line in cache[rel].splitlines():
            s = line.strip()
            if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
                continue
            # 只看字符串字面量内的 CJK
            for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', line):
                if CJK.search(m.group(1)):
                    print("  leftover %s: %s" % (rel, line.strip()[:90]))
                    left += 1
        if left == 0:
            print("clean: " + rel)
        else:
            print("  -> %d string literals still contain CJK in %s" % (left, rel))
    return 0


if __name__ == "__main__":
    sys.exit(main())
