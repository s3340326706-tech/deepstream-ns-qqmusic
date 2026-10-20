# engine/ —— 音源配置引擎（Python 参考实现）

> C++ 移植版在 `../engine_cpp/`，吃同一份 source JSON。这一份用来**快速验接口**，那一份是**要进 Switch 的**。

这是《音源配置引擎规范 v1》的**可执行版本**，用途是：

1. 验证 DSL 能不能表达真实接口（表达不了就改规范，成本 10 分钟）
2. 在真实接口上跑通 search / stream / lyric，给 C++ 移植提供语义基准
3. 校准 source JSON 的字段路径

**它不是最终产物。** 最终产物是 C++ 版（接进 borealis），这一份跑通后照着移植，只换 HTTP 与 JSON 库。

依赖：**仅 Python 标准库**。不需要 pip 装任何东西。

---

## 目录

```
engine/
    source_engine.py   ← 引擎本体（求值器 / 白名单 / HTTP / 三个操作）
    probe.py           ← 命令行调试台
```

配置与账号放在**同级以外的私有目录**，不要混进来：

```
nxhomebrew/
    engine/                       ← 可开源
    src-private/                  ← 不可开源
        account.json              ← 你的凭据
        sources/qqmusic.json      ← 音源配置
```

---

## 常用命令

```bash
# 1. 校验配置（纯离线，不联网）
python probe.py validate ../src-private/sources/qqmusic.json

# 2. 搜索（不需要登录）
python probe.py search ../src-private/sources/qqmusic.json "周杰伦 晴天" --limit 5

# 3. 看接口原始响应结构（用来找字段路径）
python probe.py raw ../src-private/sources/qqmusic.json --op stream --mid 0039MnYb0qxYhV --tree

# 4. 解流（需要凭据）
python probe.py resolve ../src-private/sources/qqmusic.json --mid 0039MnYb0qxYhV --quality flac

# 5. 拿到 URL 后验证真的能下到音频字节
python probe.py resolve ../src-private/sources/qqmusic.json --mid 0039MnYb0qxYhV --bytes

# 6. 一条龙：搜索 -> 取前 3 条解流 -> 报告成功率
python probe.py flow ../src-private/sources/qqmusic.json "周杰伦" --limit 3 --bytes
```

调试开关：`-v` 打印请求与响应（cookie 自动打码）、`--tree` 拍平响应、`--dump out.json` 存盘、`--insecure` 跳过 TLS 校验、`--no-proxy` 忽略系统代理。

---

## 凭据

**存整串 Cookie，不是拆字段。** Switch 没有浏览器，凭据只能从 PC 导入。

```bash
cp ../src-private/account.example.json ../src-private/account.json
# 把浏览器里复制的整串 Cookie 粘到 "cookie" 字段
```

或临时用：`--cookie "uin=o123456789; qm_keyst=@xxx; pgv_pvid=2497825952"`

取法：PC 浏览器登录 y.qq.com → 随便播一首 → F12 → Network → 任一请求 → Request Headers → 复制整串 `Cookie:`。

🔴 **必须包含 `pgv_pvid`** —— 它就是取流用的 `guid`。值不等于它时 `purl` **恒空且不报错**（实测 2026-10-19）。
`uin` 常是 `o123456789` 形态，配置里用 `digits()` 取纯数字。

---

## 已验证（2026-10-19 实测，真实会员账号）

| 项 | 结果 |
|---|---|
| `search`（`client_search_cp`，`new_json=1`） | ✅ 30 条/页，未登录也可搜 |
| 字段映射 `item.mid` / `name` / `singer[0].name` / `album.name` | ✅ 正确 |
| `interval` 单位 | ⚠️ 是**秒**，配置里用 `mul(...,1000)` 换毫秒 |
| `stream`（`musicu.fcg` `UrlGetVkey`） | ✅ **FLAC 3/3 成功**，206 + `magic=fLaC`，长度与 `size_flac` 一致 |
| `lyric`（`nobase64=1` + `encoding: html`） | ✅ 1388 字符，时间轴正常 |
| 配置安全闸门（host 白名单 / cookie 作用域） | ✅ 两条都实测拦截成功 |

### 两个必须记住的陷阱

**1. `filename` 要用 `media_mid`，用 `songmid` 会拿到 purl 但 CDN 404**

| filename | purl | 下载 |
|---|---|---|
| `F000<songmid>.flac` | 有 | ❌ 404 |
| `F000<media_mid>.flac` | 有 | ✅ 206 |

单独用 `resolve` 时要手动给 `--media-mid`；用 `flow` 走搜索会自动带上。
**这个失败比 purl 空更隐蔽**，容易被误判成"地址过期"。

**2. `Content-Type` 不可信**

QQ 的 FLAC 返回 `audio/x-ogg`。**解码器必须按文件头选**：
`fLaC` → FLAC，`ID3`/`0xFFFB` → MP3，`OggS` → OGG，`ftyp` → M4A。

---

## 移植到 C++ 时的对应关系

| 这里 | C++ 侧 |
|---|---|
| `urllib.request` | libcurl（**记得配 cacert.pem**，devkitPro 的 curl 没有 CA 包） |
| `json` 模块 | nlohmann/json |
| `SourceError / RequestError` | 异常或 `std::expected` |
| `do_search / do_resolve / do_lyric` | `SourceProvider` 的三个虚函数 |
| `flatten()` | 不需要，只在调试期用 |

函数白名单、host 白名单、`encrypted_markers` 三条必须原样搬过去 —— 前两条是安全边界，第三条是"不做解密"的落点。
