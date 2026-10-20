# engine_cpp/ —— 音源配置引擎（C++ 版）

与 `engine/`（Python 参考实现）**同一份 DSL、同一份 source JSON**。C++ 版是最终要进 Switch 的那一份，Python 版只用来快速验证接口。

---

## ✅ 当前状态：PC 版已编译并跑通（2026-10-19）

MSYS2 / mingw-w64 g++ 16.2.0 + libcurl 实测，`-Wall -Wextra` **零警告零错误**：

| 项目 | 结果 |
|---|---|
| `selftest`（md5 / sha1 / base64 / url_encode / digits / host_of / hash33 / gtk） | **8/8 通过**，0 项失败 |
| `flow "晴天" --limit 3 --bytes` | **3/3 全 FLAC**，`status=206`，magic `fLaC`，长度与 `file.size_flac` 一致 |
| `lyric --mid 0039MnYb0qxYhV` | 2548 字节，时间轴正常 |

与 Python 参考实现结果**完全一致**（同一份 `sources/qqmusic.json`）。

编译/运行两个坑，已修在代码里：

1. **Windows 命令行是 GBK**：`argv` 里的中文关键词是 GBK 字节，URL 编码后服务端回 GBK，`nlohmann::json::parse` 直接报 `ill-formed UTF-8 byte`。
   → `tools/probe.cpp` 加了 `acp_to_utf8()`，`main` 里把 `keyword` 转 UTF-8（仅 `_WIN32`）。Switch 侧 UI 本来就是 UTF-8，不受影响。
2. **`cc1plus` 返回 127**：直接调 `g++.exe` 但 PATH 里没有 `mingw64/bin` 时，`cc1plus` 缺 DLL 静默失败（无任何报文）。
   → 编译前必须 `export PATH="/c/msys64/mingw64/bin:$PATH"`。

`parse_body` 也从"解析失败就当字符串塞回去"改成**抛 `RequestError` 并带上字节数 / 首段 / 原始错误**，否则只会看到一句"求值结果不是数组"，排查全靠猜。

---

## 文件结构

```
engine_cpp/
├── include/
│   ├── nlohmann/json.hpp      nlohmann/json 3.11.3 单头文件（已内置，无需联网）
│   └── se/
│       ├── util.hpp           哈希 / 编码 / g_tk
│       ├── http.hpp           libcurl 薄封装
│       └── source_engine.hpp  引擎接口
├── src/
│   ├── util.cpp               md5 / sha1 / base64 / url_encode / hash33 / gtk
│   ├── http.cpp               libcurl GET/POST/Range，超时与体积上限
│   └── source_engine.cpp      校验 / 表达式求值 / 三个操作
├── tools/probe.cpp            调试台（含 selftest）
├── CMakeLists.txt             PC 构建
└── Makefile.switch            Switch 构建参考（不是拿来直接用的）
```

---

## 先在 PC 上编（已验证可行）

本机实测路径：`C:\msys64`（清华镜像装的 MSYS2，mingw64 工具链）。

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"      # 必须！否则 cc1plus 静默失败
cd engine_cpp
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    src/util.cpp src/http.cpp src/source_engine.cpp tools/probe.cpp \
    -o build/probe.exe -lcurl
```

跑：

```bash
./build/probe.exe selftest
./build/probe.exe validate ../src-private/sources/qqmusic.json
./build/probe.exe flow ../src-private/sources/qqmusic.json "晴天" --limit 3 --bytes \
                  --account ../src-private/account.json
```

`selftest` 会校验：md5 / sha1 / base64 / url_encode / digits / host_of / hash33 / gtk，全部有已知答案。

---

## 在 Switch 上编（已跑通，2026-10-19）

工具链：`C:\devkitPro`（devkitA64 g++ 16.1.0 + libnx + switch-curl + switch-mbedtls）。

**不要手写 Makefile**，用官方模板：

```bash
cp -r $DEVKITPRO/examples/switch/templates/application ./build-switch
```

模板 Makefile 改 5 处（`build-switch/Makefile` 已改好，见仓库内该文件）：

```make
TARGET   := seprobe
SOURCES  := src tools
INCLUDES := include
ROMFS    := romfs
CXXFLAGS := $(CFLAGS) -std=c++17 -frtti -fexceptions     # 必须去掉 -fno-rtti -fno-exceptions
LIBS     := -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx
```

两个死穴：

1. **`-fno-rtti -fno-exceptions` 必须去掉**：本引擎用异常报错，nlohmann 要 RTTI。
2. **`-lz` 必须加**：`switch-curl` 静态链接了 zlib 的 content-encoding，漏了会报一堆 `undefined reference to 'inflateEnd'`。

编译：

```bash
export MSYSTEM=MINGW64
/c/devkitPro/msys2/usr/bin/bash.exe -lc "cd build-switch && make sync && make -j4"
# → build-switch/seprobe.nro（1.6MB）
```

`make sync` 是自定义 target，把 `../src ../tools ../include` 同步进来，避免两份代码漂移。

根证书（**必须**，Switch 没有系统 CA 库）：

```bash
mkdir -p romfs
curl -L https://curl.se/ca/cacert.pem -o romfs/cacert.pem    # 已放进 build-switch/romfs/
```

**hbmenu 不能传命令行参数**，所以 `probe.cpp` 在 `__SWITCH__` 且无参数时会跑内置回归：
读 `sdmc:/switch/se_sources/qqmusic.json` + `account.json`，搜「晴天」，取 2 条，带字节探测。
所有分支结束后统一「按 + 退出」（否则结果一闪而过看不见）。

### 真机踩过的两个坑（2026-10-19）

**1. 初始化顺序：`nifmInitialize` 必须在 `socketInitializeDefault` 之前**

```cpp
nifmInitialize(NifmServiceType_User);   // 先
/* 轮询 nifmGetInternetConnectionStatus 直到 Connected，最多 10s */
socketInitializeDefault();              // 后
```

bsd 服务启动时要向 `nifm` 取当前网络配置（含 DNS 服务器）。顺序反了或没等连上，
`getaddrinfo` 会一直失败，报错是 `curl: (6) Could not resolve host`——和"没联网"
长得一模一样，极难区分。`probe.cpp` 的 `switch_net_wait()` 就是干这个的，
并且会打印一行 `[net] connected (wifi, signal 3, ...) ip=... dns=...` 便于定位。

**2. 控制台只能显示 ASCII，中文必须折叠**

libnx 的 `console` 用内建 8x8 点阵字体，只有 ASCII 字形。汉字是 3 字节 UTF-8，
会被逐个字节画成 3 个乱码方块，还会把 80 列的窗口撑爆 → 整屏错行（看起来像"堆在一起"）。

所以：

- 探针自己的文案全部用英文（这是**日志/异常**，不是 UI）。
- `pout()` 在 `__SWITCH__` 下把所有非 ASCII 码点折成单个 `?`，行宽永远不超。
  PC 侧不折叠，Git Bash 里中文照常显示。
- 歌词正文在 Switch 上不打印（只看字节数）。
- 未来的正式界面用 borealis + 自带 CJK 字体，中文正常显示；中文文案走 i18n 表，
  不要靠异常字符串。

---

## 与 Python 版的行为对齐

| 能力 | Python | C++ | 备注 |
|---|---|---|---|
| 配置校验（api_version / 必填 / attempts≤8 / 文件≤256KB） | ✅ | ✅ | |
| host 白名单（加载期拒绝，不是运行期跳过） | ✅ | ✅ | |
| cookie 作用域（`$cookie.*` 只对白名单 host） | ✅ | ✅ | |
| `cookie_format: raw`（整串导入） | ✅ | ✅ | |
| 表达式：`$arg/$cookie/$var/$track/$item/$resp/$now/$rand16` | ✅ | ✅ | |
| 路径取值 `a.b[0].c` | ✅ | ✅ | |
| 函数：hash33 / **gtk** / md5 / sha1 / base64 / urlencode / concat / substr / default / int / mul / digits / json | ✅ | ✅ | |
| `stream.attempts` 有序降级 | ✅ | ✅ | |
| `error` 块（接口错误码识别） | ✅ | ✅ | |
| `url_base`（相对 purl 拼 sip） | ✅ | ✅ | |
| `expires_in` → `expires_at` | ✅ | ✅ | |
| `encrypted_markers` 只能追加不能清空 | ✅ | ✅ | fail-closed |
| 歌词 `encoding: plain/base64/html/json_escaped` | ✅ | ✅ | |
| JSONP 前缀容错 | ✅ | ✅ | 取首个 `{` 之后重解 |

**两边吃同一份 `sources/qqmusic.json`**，改配置不用改代码。

---

## 已知缺口

| 项 | 说明 |
|---|---|
| `sha1` 正确性 | 只有 selftest 向量兜底；当前链路没用到（QQ 不需要，B 站用 md5） |
| Switch 真机验证 | .nro 已产出，**尚未上机跑**（TLS / 搜索 / 取流待确认） |
| 异常在 Switch 上的表现 | libnx 下未捕获异常会直接崩且没堆栈；建议外层统一 catch + `printf` |
| 包体积 | `nlohmann/json.hpp` 单头 900KB，编译后 NRO 1.6MB（可接受） |

---

## 下一步

1. 编过 PC 版 → `selftest` 全绿 → `flow` 跑通 3 首
2. 编 Switch 版 → 真机只验一条：`probe validate` + `probe flow`（先看 TLS 通不通）
3. 通了再谈解码（`dr_flac` / `dr_mp3`）与 audout
