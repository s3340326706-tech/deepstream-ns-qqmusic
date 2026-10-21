<#
    Deepstream · QQ 音乐 Cookie 一键获取

    为什么需要它：
      取流接口必须要你自己账号的 Cookie，而 QQ 音乐把关键的 qm_keyst
      设成了 HttpOnly —— 网页上 document.cookie / 书签脚本都读不到，
      F12 里翻 Network 面板又是劝退级操作。

    它怎么绕过这个门槛：
      自己拉起一个带调试端口的浏览器（独立用户目录，不影响你平时的浏览器和
      登录状态），在浏览器内部把 Cookie 直接读出来，所以你只需要「扫码登录」
      这一件事。登录状态存在本机，下次再取不用重新登录。

    用法（一般不需要手动跑，双击 Get-QQCookie.bat 就行）：
      powershell -ExecutionPolicy Bypass -File Get-QQCookie.ps1
      powershell -ExecutionPolicy Bypass -File Get-QQCookie.ps1 -Paste
      powershell -ExecutionPolicy Bypass -File Get-QQCookie.ps1 -Yes
#>
[CmdletBinding()]
param(
    # 不走浏览器：手动粘贴一整串 Cookie（或者 F12 里 Copy as cURL 的内容）
    [switch]$Paste,
    # 配合 -Paste 使用，直接把串写在命令行里（会留在历史记录里，不推荐）
    [string]$CookieString = "",
    # 全自动：检测到 SD 卡就直接写进去，不问
    [switch]$Yes,
    # 调试端口，被占用时换一个
    [int]$Port = 9777,
    # 指定浏览器，留空则自动找 Edge / Chrome
    [string]$BrowserPath = "",
    # 等待登录的最长秒数
    [int]$WaitSeconds = 600,
    # 内部自检（无界面，不碰你的账号）
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# 输出统一走 UTF-8：否则英文/日文区域的机器上中文会变成一串问号
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $OutputEncoding = New-Object System.Text.UTF8Encoding($false)
} catch { }

$ScriptDir   = $PSScriptRoot
$ProfileDir  = Join-Path $env:LOCALAPPDATA "Deepstream\qq-profile"
$AccountPath = Join-Path $ScriptDir "account.json"
$TestProfile = Join-Path $env:TEMP "deepstream-cookie-selftest"

# ─────────────────────────── 输出 ───────────────────────────
function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ""; Write-Host $text -ForegroundColor Cyan }
function Ok([string]$text)   { Write-Host "      OK  $text" -ForegroundColor Green }
function Warn([string]$text) { Write-Host "      注意 $text" -ForegroundColor Yellow }
function Bad([string]$text)  { Write-Host "      错误 $text" -ForegroundColor Red }

# ─────────────────────── 浏览器与调试端口 ───────────────────────

function Find-Browser {
    if ($BrowserPath) {
        if (Test-Path $BrowserPath) { return $BrowserPath }
        throw "指定的浏览器不存在：$BrowserPath"
    }
    $cands = @(
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw "没找到 Edge 或 Chrome。装一个，或者用 -Paste 手动粘贴 Cookie。"
}

# 读调试端口的 HTTP 端点（不配置代理：本地回环绝不能走代理）
function Get-CdpJson([string]$path, [int]$timeoutMs = 1500) {
    try {
        $req = [System.Net.WebRequest]::Create("http://127.0.0.1:$Port$path")
        $req.Proxy   = $null
        $req.Timeout = $timeoutMs
        $resp = $req.GetResponse()
        $reader = New-Object System.IO.StreamReader($resp.GetResponseStream(), [System.Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
        $reader.Close()
        $resp.Close()
        return $text
    } catch {
        return $null
    }
}

function Get-CdpWsUrl([int]$waitSeconds = 45) {
    $deadline = (Get-Date).AddSeconds($waitSeconds)
    while ((Get-Date) -lt $deadline) {
        $text = Get-CdpJson "/json/version"
        if ($text) {
            try {
                $url = ($text | ConvertFrom-Json).webSocketDebuggerUrl
                if ($url) { return $url }
            } catch { }
        }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

function Start-DebugBrowser([string]$exe, [string]$profile, [switch]$Headless) {
    if (-not (Test-Path $profile)) { New-Item -ItemType Directory -Path $profile -Force | Out-Null }
    $argLine = '--remote-debugging-port={0} --user-data-dir="{1}" --no-first-run --no-default-browser-check --disable-sync' -f $Port, $profile
    if ($Headless) {
        $argLine = '--headless=new ' + $argLine + ' about:blank'
    } else {
        $argLine = $argLine + ' "https://y.qq.com/"'
    }
    Start-Process -FilePath $exe -ArgumentList $argLine | Out-Null
}

# ─────────────────────── 极简 CDP 客户端 ───────────────────────
# 只要一条命令一问一答，不引第三方 WebSocket 库。

function Connect-Cdp([string]$wsUrl) {
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ws.Options.Proxy = $null
    $task = $ws.ConnectAsync([Uri]$wsUrl, [System.Threading.CancellationToken]::None)
    if (-not $task.Wait(8000)) { throw "连接浏览器调试端口超时" }
    return $ws
}

function Send-Cdp($ws, [int]$id, [string]$method, $params) {
    if ($null -eq $params) {
        $payload = @{ id = $id; method = $method }
    } else {
        $payload = @{ id = $id; method = $method; params = $params }
    }
    $json  = $payload | ConvertTo-Json -Depth 12 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $seg   = New-Object System.ArraySegment[byte] -ArgumentList @(, $bytes)
    $task  = $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [System.Threading.CancellationToken]::None)
    if (-not $task.Wait(8000)) { throw "向浏览器发送指令超时" }
}

function Receive-Cdp($ws, [int]$timeoutMs = 20000) {
    $buf = New-Object byte[] 65536
    $sb  = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ($true) {
        $seg  = New-Object System.ArraySegment[byte] -ArgumentList @(, $buf)
        $task = $ws.ReceiveAsync($seg, [System.Threading.CancellationToken]::None)
        $remain = [int]($deadline - (Get-Date)).TotalMilliseconds
        if ($remain -lt 0) { $remain = 0 }
        if (-not $task.Wait($remain)) { throw "等待浏览器返回超时" }
        $r = $task.Result
        if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return $null }
        [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
        if ($r.EndOfMessage) { break }
    }
    return $sb.ToString()
}

# 发一条 CDP 命令并等它的结果；中途的事件消息直接丢掉
function Invoke-Cdp($ws, [string]$method, $params, [int]$id = 1) {
    Send-Cdp $ws $id $method $params
    while ($true) {
        $text = Receive-Cdp $ws
        if ($null -eq $text) { throw "浏览器调试连接被关闭" }
        $msg = $text | ConvertFrom-Json
        if ($msg.id -ne $id) { continue }
        if ($msg.error) { throw ("浏览器返回错误：" + ($msg.error | ConvertTo-Json -Compress)) }
        return $msg.result
    }
}

function Close-Cdp($ws) {
    if ($null -eq $ws) { return }
    try {
        $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "", [System.Threading.CancellationToken]::None).Wait(2000) | Out-Null
    } catch { }
    try { $ws.Dispose() } catch { }
}

# ─────────────────────── Cookie 处理 ───────────────────────

# 浏览器里所有 qq.com 域下的 Cookie，按浏览器原始顺序拼成一条 Cookie 头
function Build-CookieString($cookies) {
    $parts = New-Object System.Collections.Generic.List[string]
    $skipped = 0
    foreach ($c in $cookies) {
        if ($null -eq $c.name -or $null -eq $c.value) { continue }
        if ($c.name -eq "") { continue }
        if ($c.domain -notmatch 'qq\.com$') { continue }
        # Cookie 头只能是 ASCII，含非 ASCII 的宁可不要
        if ($c.value -match '[^\x20-\x7E]') { $skipped++; continue }
        $parts.Add(("{0}={1}" -f $c.name, $c.value))
    }
    return @{ text = ($parts -join "; "); count = $parts.Count; skipped = $skipped }
}

function Get-CookieMap([string]$cookie) {
    $map = @{}
    foreach ($part in $cookie.Split(";")) {
        $p = $part.Trim()
        if (-not $p) { continue }
        $i = $p.IndexOf("=")
        if ($i -le 0) { continue }
        $k = $p.Substring(0, $i).Trim()
        $v = $p.Substring($i + 1).Trim()
        # 同名可能出现多次（不同 domain/path），空的不能盖掉后面有值的
        if (-not $map.ContainsKey($k)) {
            $map[$k] = $v
        } elseif (-not $map[$k] -and $v) {
            $map[$k] = $v
        }
    }
    return $map
}

function Get-Digits([string]$s) {
    if (-not $s) { return "" }
    return ($s -replace '[^0-9]', '')
}

# 检查取流真正需要的几样东西
function Test-CookieHealth([string]$cookie) {
    $map = Get-CookieMap $cookie
    $key  = ""
    foreach ($n in @("qm_keyst", "qqmusic_key", "music_key", "p_skey", "skey")) {
        if ($map.ContainsKey($n) -and $map[$n]) { $key = $n; break }
    }
    $guid = ""
    foreach ($n in @("pgv_pvid", "qqmusic_guid", "guid")) {
        if ($map.ContainsKey($n) -and $map[$n]) { $guid = $n; break }
    }
    $uin = ""
    foreach ($n in @("uin", "p_uin", "wxuin")) {
        if ($map.ContainsKey($n) -and (Get-Digits $map[$n])) { $uin = Get-Digits $map[$n]; break }
    }
    return [pscustomobject]@{
        Ok      = [bool]($key -and $guid -and $uin)
        KeyName = $key
        GuidName = $guid
        Guid    = if ($guid) { $map[$guid] } else { "" }
        Uin     = $uin
        Count   = $map.Count
    }
}

# ─────────────────────── 落盘 ───────────────────────

function Save-Account([string]$cookie, [string]$path) {
    if (Test-Path $path) { Copy-Item $path "$path.bak" -Force }
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $obj = [ordered]@{
        cookie    = $cookie
        _saved_at = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
        _source   = "Get-QQCookie"
    }
    $json = $obj | ConvertTo-Json -Depth 4
    # UTF-8 无 BOM：Switch 上的 JSON 解析器不认 BOM
    [System.IO.File]::WriteAllText($path, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Find-SwitchRoots {
    $roots = @()
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        try {
            if (-not $d.IsReady) { continue }
            $root = $d.RootDirectory.FullName
            if (Test-Path (Join-Path $root "switch")) { $roots += $root }
        } catch { }
    }
    return $roots
}

function Copy-ToSwitch([string]$root, [string]$srcFile) {
    $dstDir = Join-Path $root "switch\se_sources"
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
    $dst = Join-Path $dstDir "account.json"
    Copy-Item $srcFile $dst -Force
    return $dst
}

# ─────────────────────── 自检 ───────────────────────

function Invoke-SelfTest([string]$exe) {
    Say "自检：headless 浏览器 + 注入假 Cookie + 读回 + 落盘" DarkGray
    if (Test-Path $TestProfile) { Remove-Item $TestProfile -Recurse -Force -ErrorAction SilentlyContinue }
    try {
        Start-DebugBrowser $exe $TestProfile -Headless
        $wsUrl = Get-CdpWsUrl 40
        if (-not $wsUrl) { throw "调试端口没起来" }
        $ws = Connect-Cdp $wsUrl

        $seed = @(
            @{ name = "qm_keyst"; value = "@SELFTEST_KEY"; domain = ".qq.com"; path = "/"; secure = $true },
            @{ name = "pgv_pvid"; value = "1234567890";    domain = ".qq.com"; path = "/" },
            @{ name = "uin";       value = "o0123456789";  domain = ".qq.com"; path = "/" },
            @{ name = "noise";     value = "x";            domain = ".example.com"; path = "/" }
        )
        Invoke-Cdp $ws "Storage.setCookies" @{ cookies = $seed } | Out-Null
        $res = Invoke-Cdp $ws "Storage.getCookies" $null
        Close-Cdp $ws

        $built = Build-CookieString $res.cookies
        $ck = $built.text
        $health = Test-CookieHealth $ck

        $fail = @()
        if ($res.cookies.Count -lt 3) { $fail += "读回的 Cookie 数量不对（$($res.cookies.Count)）" }
        if ($ck -notmatch 'qm_keyst=@SELFTEST_KEY') { $fail += "没拼出 qm_keyst" }
        if ($ck -notmatch 'pgv_pvid=1234567890') { $fail += "没拼出 pgv_pvid" }
        if ($ck -match 'example\.com|x=;') { $fail += "非 qq.com 域的 Cookie 没被过滤" }
        if (-not $health.Ok) { $fail += "健康检查没通过" }
        if ($health.Uin -ne "0123456789") { $fail += "uin 解析错误（$($health.Uin)）" }

        $tmp = Join-Path $env:TEMP "deepstream-selftest-account.json"
        Save-Account $ck $tmp
        $back = (Get-Content $tmp -Raw -Encoding UTF8) | ConvertFrom-Json
        if ($back.cookie -ne $ck) { $fail += "写盘后读回不一致" }
        $head = [System.IO.File]::ReadAllBytes($tmp)[0..2] -join ","
        if ($head -eq "239,187,191") { $fail += "JSON 带了 BOM" }
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue

        if ($fail.Count -gt 0) {
            Say ("SELFTEST_FAIL: " + ($fail -join " | ")) Red
            return 1
        }
        Say ("SELFTEST_OK  cookies=$($res.cookies.Count) chars=$($ck.Length) uin=$($health.Uin)") Green
        return 0
    } catch {
        Say ("SELFTEST_FAIL: " + $_.Exception.Message) Red
        return 1
    } finally {
        # 把自检用的 headless 进程收掉，别在后台留一堆
        try {
            Get-CimInstance Win32_Process -Filter "Name='msedge.exe' OR Name='chrome.exe'" -ErrorAction SilentlyContinue |
                Where-Object { $_.CommandLine -like "*deepstream-cookie-selftest*" } |
                ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
            Start-Sleep -Milliseconds 400
        } catch { }
        Remove-Item $TestProfile -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ─────────────────────── 主流程 ───────────────────────

function Show-Banner {
    Write-Host ""
    Say "==================================================" Cyan
    Say "   Deepstream · QQ 音乐 Cookie 一键获取" Cyan
    Say "==================================================" Cyan
}

function Get-CookieFromPaste {
    $raw = $CookieString
    if (-not $raw) {
        Say ""
        Say "把整串 Cookie 粘进来，然后回车。" Gray
        Say "（也可以粘 F12 里 Copy as cURL 得到的那一整行，我会自己挑出来）" DarkGray
        Write-Host "> " -NoNewline -ForegroundColor Gray
        $raw = Read-Host
    }
    if (-not $raw) { throw "没读到内容" }

    # 从 cURL / 请求头里把 cookie 那一段挖出来
    if ($raw -match '(?i)cookie:\s*([^\r\n'']+)') {
        $raw = $matches[1]
    }
    $raw = $raw.Trim().Trim('"').Trim("'").Trim()
    if ($raw -notmatch '=') { throw "这看起来不是 Cookie（里面没有 name=value）" }
    return $raw
}

function Main {
    Show-Banner

    $exe = Find-Browser
    $lastStep = "[3/3]"

    if ($Paste) {
        $lastStep = "[2/2]"
        Step "[1/2] 手动粘贴模式"
        $ck = Get-CookieFromPaste
    } else {
        Step "[1/3] 启动带调试端口的浏览器"
        Say "      程序: $exe" DarkGray
        Say "      登录状态目录: $ProfileDir" DarkGray
        Say "      （独立目录，不会动你平时浏览器的登录状态）" DarkGray

        $wsUrl = $null
        $existing = Get-CdpJson "/json/version"
        if ($existing) {
            $wsUrl = ($existing | ConvertFrom-Json).webSocketDebuggerUrl
            Ok "复用已经在跑的调试浏览器（端口 $Port）"
        } else {
            Start-DebugBrowser $exe $ProfileDir
            $wsUrl = Get-CdpWsUrl 60
            if (-not $wsUrl) { throw "浏览器调试端口没起来。换个端口试试：-Port 9778" }
            Ok "浏览器已启动（端口 $Port）"
        }

        Step "[2/3] 等你在窗口里登录"
        Say "      在弹出的浏览器里点右上角「登录」，用 QQ 或微信扫码" White
        Say "      之前登录过的话，这一步会自动跳过" DarkGray
        Say ""

        $ws = Connect-Cdp $wsUrl
        $ck = $null
        $health = $null
        $deadline = (Get-Date).AddSeconds($WaitSeconds)
        $lastDot = Get-Date
        try {
            while ($true) {
                $res = Invoke-Cdp $ws "Storage.getCookies" $null
                $built = Build-CookieString $res.cookies
                $cookie = $built.text
                if ($cookie) {
                    $h = Test-CookieHealth $cookie
                    if ($h.Ok) { $ck = $cookie; $health = $h; break }
                }
                if ((Get-Date) -gt $deadline) {
                    throw "等了 $WaitSeconds 秒还没检测到登录。请确认浏览器里已经登录成功，然后重新运行本脚本。"
                }
                if (((Get-Date) - $lastDot).TotalSeconds -ge 3) {
                    Write-Host "." -NoNewline -ForegroundColor DarkGray
                    $lastDot = Get-Date
                }
                Start-Sleep -Milliseconds 800
            }
        } finally {
            Close-Cdp $ws
        }
        Write-Host ""
        Ok "检测到登录状态"
    }

    if (-not $health) { $health = Test-CookieHealth $ck }

    Step "$lastStep 检查并写入"
    Say ("      Cookie 长度 : {0} 字符 / {1} 项" -f $ck.Length, $health.Count) DarkGray
    Say ("      账号 uin    : {0}" -f $(if ($health.Uin) { $health.Uin } else { "(没找到)" })) DarkGray
    if ($health.KeyName) {
        Ok "取流密钥 $($health.KeyName) 已就位"
    } else {
        Warn "没找到 qm_keyst / qqmusic_key，可能放不出歌"
    }
    if ($health.GuidName) {
        Ok "设备标识 $($health.GuidName) = $($health.Guid)（缺了必然 404）"
    } else {
        Warn "没找到 pgv_pvid，取流会全部 404 —— 这是最常见的坑"
    }

    Save-Account $ck $AccountPath
    Ok "已写入 $AccountPath"
    if (Test-Path "$AccountPath.bak") { Say "      （旧文件已备份为 account.json.bak）" DarkGray }

    # 找 SD 卡
    $roots = Find-SwitchRoots
    if ($roots.Count -gt 0) {
        foreach ($root in $roots) {
            $target = $null
            if ($Yes) {
                $target = Copy-ToSwitch $root $AccountPath
                Ok "已复制到 SD 卡: $target"
            } else {
                Say ""
                Write-Host ("检测到 Switch 目录: " + (Join-Path $root "switch")) -ForegroundColor Yellow -NoNewline
                $ans = Read-Host " 要直接写进去吗？(Y/n)"
                if ($ans -eq "" -or $ans -match '^(?i)y') {
                    $target = Copy-ToSwitch $root $AccountPath
                    Ok "已复制到 SD 卡: $target"
                }
            }
        }
    } else {
        Say ""
        Warn "没检测到已经插着的 SD 卡。请手动把 account.json 放到："
        Say  "      SD 卡根目录\switch\se_sources\account.json" White
    }

    Write-Host ""
    Say "==================================================" Cyan
    Say "  完成。插回 SD 卡，启动 Deepstream 就能用了。" Green
    if (-not $Paste) {
        Say "  浏览器窗口可以关掉，登录状态已经记在本机，" DarkGray
        Say "  下次 Cookie 过期时重跑本脚本不用再扫码。" DarkGray
    }
    Say "  Cookie 大约几周到几个月会过期。哪天歌单空了或者" DarkGray
    Say "  全部 404，先重跑一次本脚本，再怀疑别的东西。" DarkGray
    Say "==================================================" Cyan
    Write-Host ""
    return 0
}

try {
    if ($SelfTest) { exit (Invoke-SelfTest (Find-Browser)) }
    exit (Main)
} catch {
    Write-Host ""
    Bad $_.Exception.Message
    Write-Host ""
    Say "如果一直失败，可以改用手动模式：" Yellow
    Say "  powershell -ExecutionPolicy Bypass -File Get-QQCookie.ps1 -Paste" White
    Write-Host ""
    exit 1
}
