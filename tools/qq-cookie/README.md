# 一键获取 QQ 音乐 Cookie

Deepstream 起播前需要你自己账号的 Cookie。手工做法是 F12 → Network → 翻请求头，
这一步劝退了相当多的人，而且 **复制漏了 `pgv_pvid` 不会报错，只会所有歌都 404**，
排查起来极其痛苦。

这个脚本把这件事压到「扫码登录一次」：

```
双击 Get-QQCookie.bat
  → 自动弹出一个浏览器窗口（独立登录状态，不碰你平时的浏览器）
  → 你扫码登录 QQ 音乐
  → 脚本自动生成 account.json
  → 检测到 SD 卡就问你要不要直接写进去
```

不用装 Python，不用装任何东西，用 Windows 自带的 PowerShell 5.1 就行。

## 为什么必须走浏览器

QQ 音乐把取流用的 `qm_keyst` 设成了 **HttpOnly**，网页里的 `document.cookie`
和任何书签脚本都读不到它。所以脚本的做法是给浏览器开一个本地调试端口，
从浏览器内部把 Cookie 读出来——这是唯一既自动又能拿到 HttpOnly 的途径。

## 用法

| 场景 | 命令 |
| --- | --- |
| 正常获取（推荐） | 双击 `Get-QQCookie.bat` |
| 全自动，SD 卡直接写 | `Get-QQCookie.bat -Yes` |
| 手动粘贴 Cookie / cURL | `Get-QQCookie.bat -Paste` |
| 调试端口冲突 | `Get-QQCookie.bat -Port 9778` |
| 用指定的浏览器 | `Get-QQCookie.bat -BrowserPath "D:\Chrome\chrome.exe"` |

要绕过 PowerShell 执行策略，`Get-QQCookie.bat` 已经替你加好了
`-ExecutionPolicy Bypass`，直接双击即可。

## 它会怎么处理你的数据

- 生成的 `account.json` 只写在本机：脚本所在目录，以及你确认过的 SD 卡。
- 登录状态存在 `%LOCALAPPDATA%\Deepstream\qq-profile`，**下次不用重新扫码**。
  不想要了就删掉这个目录。
- 脚本不联网、不上传、不读取任何和 qq.com 无关的 Cookie。
- 旧文件会被备份成 `account.json.bak`，写错了能退回去。

`account.json` 等同于你的账号凭据。不要发给任何人，不要提交到任何仓库。

## 失败了怎么办

| 现象 | 原因 / 处理 |
| --- | --- |
| 找不到 Edge 或 Chrome | 装一个，或者用 `-Paste` 手动粘 |
| 调试端口没起来 | 换端口：`-Port 9778`；或先关掉脚本自己开的那个浏览器窗口 |
| 一直显示等待登录 | 确认那个新窗口里真的登录成功了（右上角显示头像） |
| 生成成功但播放全 404 | 说明 `pgv_pvid` 没拿到，重跑一次；还是不行就用 `-Paste` |
| 用一段时间后歌单空了 | Cookie 过期，重跑脚本即可 |

手动模式的取法：`y.qq.com` 登录 → F12 → Network → 任意请求 →
Request Headers 里以 `Cookie:` 开头的那一整行复制粘贴。
`-Paste` 也接受 **Copy as cURL** 的整行内容，脚本会自己把 Cookie 抠出来。

## 非 Windows 用户

脚本依赖 PowerShell + Edge/Chrome 的 CDP，目前只做了 Windows。
macOS / Linux 暂时只能用手动方式（F12 复制 Cookie 串），写进
`account.json` 的 `cookie` 字段即可。
