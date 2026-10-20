# 第三方组件声明

本项目不是从零写的，站在这些项目的肩膀上。各组件的许可证如下，分发二进制时必须一并保留本文件。

## 直接链接进 `deepstream.nro` 的库

| 组件 | 版本 / 来源 | 许可证 | 用途 |
|---|---|---|---|
| **borealis** | https://github.com/xfangfang/borealis （本地副本基于 commit `5f08b28`） | **Apache-2.0** | Switch 上的 UI 框架、输入、渲染封装 |
| **faad2** | https://github.com/knik0/faad2 （2.11.x） | **GPL-2.0-or-later** | AAC / m4a 解码 |
| **libFLAC** | devkitPro portlibs (`switch-flac`) | BSD-3-Clause | 无损流式解码 |
| **mpg123** | devkitPro portlibs (`switch-mpg123`) | LGPL-2.1 | MP3 解码 |
| **libcurl** | devkitPro portlibs (`switch-curl`) | curl（MIT 风格） | HTTP 下载 |
| **mbedTLS** | devkitPro portlibs (`switch-mbedtls`) | Apache-2.0 | TLS |
| **zlib** | devkitPro portlibs | zlib | 压缩 |
| **libnx** | devkitPro | ISC | Switch 系统调用封装 |
| **Material Icons** | Google | Apache-2.0 | 界面图标字形 |

## 关于 faad2 与 GPL 的硬约束

faad2 的许可证原文写得很直白：

> Any non-GPL usage of this software or parts of this software is strictly forbidden.

它的源码头部同时声明 `either version 2 of the License, or (at your option) any later version`。

因此：

1. **只要 `deepstream.nro` 里链接了 faad2，整个可执行文件就必须按 GPL 分发**，并且必须提供完整对应源码。本项目选择 **GPL-3.0**。
2. 选 GPL-3.0 而不是 GPL-2.0 的原因是 **borealis 是 Apache-2.0**，Apache-2.0 与 GPL-2.0 存在专利条款冲突，与 GPL-3.0 兼容。这是唯一不需要换库就能合规的组合。
3. 如果你想做一个**闭源**的分发版本，必须先把 faad2 换掉。可替代方案是 FDK-AAC（`libfdk-aac`，许可证更宽松但体积更大，且需要自己交叉编译）。换库后 `app/src/player/audio_engine.cpp` 里 `open_aac()` 那一段需要重写，`mp4_demux` 产出的裸帧可以直接复用。

## 本项目的原创部分

`app/`、`engine_cpp/`、`third_party/faad2/` 之外的所有自有代码，同样以 GPL-3.0 授权。

其中的 `third_party/faad2/` 是上游源码的原样副本（只去掉不需要的示例与构建脚本），版权归原作者。
