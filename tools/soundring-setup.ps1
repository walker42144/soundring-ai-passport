<#
.SYNOPSIS
    声环 SoundRing 首次配置工具（Windows / PowerShell 7+）：写入 Wi-Fi 与 Spotify 授权。

.DESCRIPTION
    面向 FoloToy AI Passport（ESP32-C3）的首次配置工具，只用 Windows 自带 .NET，无需第三方模块。
    流程：
      1. 打开设备的 USB 串口（VID 0x303A），等待设备广播配对帧 SRPAIR1；
      2. 用 8 位配对码 + nonce 派生 PBKDF2-HMAC-SHA256 密钥，并完成 HMAC hello 握手；
      3. 用 AES-256-GCM 加密写入 Wi-Fi 凭据（n=1）；
      4. 在本机 127.0.0.1 起回调服务，用 Authorization Code + PKCE（S256）完成 Spotify 授权；
      5. 用 AES-256-GCM 加密写入令牌（n=2），随后发送 bye 结束会话。

    凭据只在内存中处理：不打印、不落盘、不写日志。工具不需要管理员权限，也不修改系统配置。

.PARAMETER Port
    设备串口名，例如 COM5。省略时自动探测 VID_303A 的串口。

.PARAMETER ClientId
    Spotify 应用的 Client ID（32 位十六进制）。非 -SelfTest 时必填。

.PARAMETER Ssid
    Wi-Fi 名称（SSID）。省略时交互输入。

.PARAMETER Password
    Wi-Fi 密码。省略时交互输入（输入过程不回显）。

.PARAMETER Redirect
    OAuth 回调地址，必须由用户提供并指向本机 http 地址。
    该地址需要与 Spotify 开发者后台中登记的 Redirect URI 完全一致。

.PARAMETER Scopes
    请求的 Spotify 权限，空格分隔。

.PARAMETER SelfTest
    只跑本地自检（PBKDF2 标准向量、AES-256-GCM 往返与篡改检测、HMAC-SHA256），
    不连接设备、不联网，用退出码表示结果。

.PARAMETER TimeoutSec
    等待设备广播、等待浏览器授权的超时时间（秒），默认 240。

.EXAMPLE
    pwsh -NoProfile -File tools/soundring-setup.ps1 -SelfTest

.EXAMPLE
    pwsh -NoProfile -File tools/soundring-setup.ps1 -ClientId 0123456789abcdef0123456789abcdef -Ssid MyWiFi

.EXAMPLE
    pwsh -NoProfile -File tools/soundring-setup.ps1 -Port COM5 -ClientId 0123456789abcdef0123456789abcdef -Ssid MyWiFi

.NOTES
    先在设备上进入 设置 → 电脑配对，再运行本工具。设备配对会话 10 分钟有效，配对码连续错误 5 次会结束会话。
#>

#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Port,
    [string]$ClientId,
    [string]$Ssid,
    [string]$Password,
    [string]$Redirect,
    [string]$Scopes = 'user-read-playback-state user-modify-playback-state playlist-read-private playlist-read-collaborative user-read-recently-played user-library-read user-library-modify',
    [switch]$SelfTest,
    [switch]$AuthOnly,
    [switch]$WifiOnly,
    [int]$TimeoutSec = 240
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$script:RxPending = ''
$script:SrKey = $null

# ---------------------------------------------------------------- 通用工具

# PowerShell 无法把 byte[] 绑定到 Span<byte> 形参（CryptographicOperations.ZeroMemory
# 会抛 "无法转换为类似 ByRef 的类型 System.Span`1"），这里用 Array.Clear 达到同样效果。
function Clear-Bytes {
    param([byte[]]$Bytes)
    if ($null -ne $Bytes -and $Bytes.Length -gt 0) {
        [System.Array]::Clear($Bytes, 0, $Bytes.Length)
    }
}

function New-RandomHex {
    param([int]$ByteCount)
    $bytes = [byte[]]::new($ByteCount)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToHexString($bytes).ToLowerInvariant()
}

function Get-Base64UrlSha256 {
    param([string]$Text)
    $hash = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::ASCII.GetBytes($Text))
    return [Convert]::ToBase64String($hash).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

function Get-HmacSha256 {
    param([byte[]]$Key, [string]$Text)
    $hmac = [System.Security.Cryptography.HMACSHA256]::new($Key)
    try {
        return $hmac.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
    } finally {
        $hmac.Dispose()
    }
}

# 设备端：PBKDF2-HMAC-SHA256(password=8 位配对码, salt=nonce 16 字节, 20000 次, 32 字节)
function Get-SrKey {
    param([string]$Code, [byte[]]$Nonce)
    return [System.Security.Cryptography.Rfc2898DeriveBytes]::Pbkdf2(
        $Code, $Nonce, 20000,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256, 32)
}

function Protect-SrPayload {
    param([byte[]]$Key, [string]$Aad, [string]$PlainText)
    $iv = [byte[]]::new(12)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($iv)
    $plain = [System.Text.Encoding]::UTF8.GetBytes($PlainText)
    $cipher = [byte[]]::new($plain.Length)
    $tag = [byte[]]::new(16)
    $aes = [System.Security.Cryptography.AesGcm]::new($Key, 16)
    try {
        $aes.Encrypt($iv, $plain, $cipher, $tag, [System.Text.Encoding]::UTF8.GetBytes($Aad))
    } finally {
        $aes.Dispose()
        Clear-Bytes $plain
    }
    return [pscustomobject]@{
        IvHex  = [Convert]::ToHexString($iv).ToLowerInvariant()
        CtHex  = [Convert]::ToHexString($cipher).ToLowerInvariant()
        TagHex = [Convert]::ToHexString($tag).ToLowerInvariant()
    }
}

function Unprotect-SrPayload {
    param([byte[]]$Key, [string]$Aad, [string]$IvHex, [string]$CtHex, [string]$TagHex)
    $iv = [Convert]::FromHexString($IvHex)
    $cipher = [Convert]::FromHexString($CtHex)
    $tag = [Convert]::FromHexString($TagHex)
    $plain = [byte[]]::new($cipher.Length)
    $aes = [System.Security.Cryptography.AesGcm]::new($Key, 16)
    try {
        $aes.Decrypt($iv, $cipher, $tag, $plain, [System.Text.Encoding]::UTF8.GetBytes($Aad))
        return [System.Text.Encoding]::UTF8.GetString($plain)
    } finally {
        $aes.Dispose()
        Clear-Bytes $plain
    }
}

# ---------------------------------------------------------------- 串口

function Find-PassportPort {
    $found = @()
    try {
        $devices = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.PNPDeviceID -and $_.PNPDeviceID -match 'VID_303A' }
    } catch {
        $devices = @()
    }
    foreach ($device in $devices) {
        if ($device.Name -match '\((COM\d+)\)') { $found += $matches[1] }
    }
    $found = @($found | Select-Object -Unique)
    if ($found.Count -eq 1) { return $found[0] }
    if ($found.Count -gt 1) {
        throw "检测到多个 VID_303A 串口（$($found -join '、')）。请用 -Port 指定设备对应的那个串口。"
    }
    return $null
}

function Send-DeviceLine {
    param([System.IO.Ports.SerialPort]$SerialPort, [string]$Line)
    try {
        $SerialPort.Write($Line + "`r`n")
    } catch {
        throw "写入串口失败：$($_.Exception.Message)。请检查 USB 连接后重新运行本工具。"
    }
}

# 不做阻塞式 ReadLine：轮询可用字节，自己切行，能处理粘包与超长行。
function Wait-DeviceLine {
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [string[]]$Prefixes,
        [int]$TimeoutSec
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $SerialPort.IsOpen) {
            throw '串口已关闭，设备可能被拔下。请重新连接设备，并在设备上重新进入 设置 → 电脑配对。'
        }
        $chunk = ''
        try {
            if ($SerialPort.BytesToRead -gt 0) { $chunk = $SerialPort.ReadExisting() }
        } catch {
            throw "读取串口失败：$($_.Exception.Message)。请检查 USB 连接后重新运行本工具。"
        }

        if ($chunk) {
            $script:RxPending += $chunk
            $parts = $script:RxPending -split "`n"
            if ($parts.Length -gt 1) {
                for ($i = 0; $i -lt $parts.Length - 1; $i++) {
                    $line = $parts[$i].TrimEnd("`r")
                    if (-not $line) { continue }
                    foreach ($prefix in $Prefixes) {
                        # 设备输出形如 "SROK {...}"；ESP 日志行不含这些前缀，直接被忽略。
                        if ($line.StartsWith($prefix + ' ', [StringComparison]::Ordinal)) {
                            # 只保留最后一个未完成的行：已匹配行及其后完整行都丢弃，
                            # 避免下一次等待把上一条命令的旧回复当成新回复。
                            $script:RxPending = $parts[$parts.Length - 1]
                            return [pscustomobject]@{
                                Prefix = $prefix
                                Json   = $line.Substring($prefix.Length + 1).Trim()
                            }
                        }
                    }
                }
                $script:RxPending = $parts[$parts.Length - 1]
            }
            if ($script:RxPending.Length -gt 8192) {
                # 单行超长（没有换行）时只保留尾部，避免缓冲无限增长。
                $script:RxPending = $script:RxPending.Substring($script:RxPending.Length - 1024)
            }
        } else {
            Start-Sleep -Milliseconds 40
        }
    }
    throw "等待设备响应超时（$TimeoutSec 秒）。请确认设备仍停在配对界面；如果配对已结束（例如配对码连续错误 5 次），请在设备上重新进入 设置 → 电脑配对。"
}

function Get-DeviceErrorHint {
    param([string]$Reason)
    switch ($Reason) {
        'bad_code' { return '设备校验配对码失败。请在设备上重新进入 设置 → 电脑配对（连续 5 次失败后设备会结束本次配对），再运行本工具。' }
        'need_hello' { return '设备要求先完成握手，会话可能已重建。请在设备上重新进入 设置 → 电脑配对后重试。' }
        'bad_sequence' { return '消息序号不同步。请在设备上重新进入 设置 → 电脑配对后重试。' }
        'bad_cipher' { return '设备无法解密密文，通常是因为配对码或 nonce 已变化。请在设备上重新进入 设置 → 电脑配对后重试。' }
        'wifi_rejected' { return '设备拒绝保存 Wi-Fi：请确认 SSID 与密码正确、路由器为 2.4GHz 频段，然后重新运行本工具。' }
        'auth_rejected' { return '设备拒绝保存 Spotify 授权。请重新授权后再次运行本工具。' }
        'bad_request' { return '设备认为请求格式不正确。请确认设备固件与本工具版本匹配。' }
        'line_too_long' { return '设备行缓冲上限不足，无法接收这一帧（通常发生在 Spotify 授权写入时）。请更新到最新固件后重试。' }
        default { return "设备返回错误（$Reason）。请在设备上重新进入 设置 → 电脑配对后重试。" }
    }
}

function Resolve-DeviceReply {
    param($Reply, [string]$Cmd)
    $obj = $null
    try { $obj = $Reply.Json | ConvertFrom-Json -AsHashtable } catch { $obj = $null }
    if ($Reply.Prefix -eq 'SRERR') {
        $reason = if ($obj -and $obj['error']) { [string]$obj['error'] } else { 'unknown' }
        throw (Get-DeviceErrorHint -Reason $reason)
    }
    if (-not $obj -or [string]$obj['cmd'] -ne $Cmd) {
        throw '设备返回了无法识别的响应。请重新运行本工具；若持续出现，请更新设备固件。'
    }
    return $obj
}

# 读取设备联网状态：SROK {"cmd":"status","note":"connected=1,time=1,ip=192.168.x.x"}
function Get-DeviceStatus {
    param([System.IO.Ports.SerialPort]$SerialPort)
    Send-DeviceLine -SerialPort $SerialPort -Line '{"v":1,"cmd":"status"}'
    $reply = Wait-DeviceLine -SerialPort $SerialPort -Prefixes @('SROK', 'SRERR') -TimeoutSec 6
    $obj = Resolve-DeviceReply -Reply $reply -Cmd 'status'
    $note = [string]$obj['note']
    return [pscustomobject]@{
        Connected = ($note -match 'connected=1')
        TimeValid = ($note -match 'time=1')
        AuthReady = ($note -match 'auth=1')
        Ip        = if ($note -match 'ip=([^,]*)') { $Matches[1] } else { '' }
    }
}

function Invoke-SecureCommand {
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [byte[]]$Key,
        [string]$Cmd,
        [int]$Seq,
        [string]$PlainJson,
        [int]$TimeoutSec = 60
    )
    $aad = "sr1|$Cmd|$Seq"
    $payload = Protect-SrPayload -Key $Key -Aad $aad -PlainText $PlainJson
    $line = '{"v":1,"cmd":"' + $Cmd + '","n":' + $Seq + ',"iv":"' + $payload.IvHex +
            '","ct":"' + $payload.CtHex + '","tag":"' + $payload.TagHex + '"}'
    Send-DeviceLine -SerialPort $SerialPort -Line $line
    $reply = Wait-DeviceLine -SerialPort $SerialPort -Prefixes @('SROK', 'SRERR') -TimeoutSec $TimeoutSec
    return Resolve-DeviceReply -Reply $reply -Cmd $Cmd
}

# ---------------------------------------------------------------- OAuth 回调页

function Get-CallbackHtml {
    param([string]$Title, [string]$Message)
    return @"
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>$Title</title>
<style>
:root{color-scheme:light dark;--bg:#f6f7f8;--card:#fff;--fg:#1a1c1f;--muted:#5c6370;--line:#e3e5e8}
@media (prefers-color-scheme:dark){:root{--bg:#131417;--card:#1b1d21;--fg:#eef0f2;--muted:#a2a9b4;--line:#2a2d33}}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--fg);font:16px/1.6 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}
main{width:min(92vw,420px);background:var(--card);border:1px solid var(--line);border-radius:14px;padding:28px 26px}
h1{margin:0 0 8px;font-size:19px;font-weight:600}
p{margin:0;color:var(--muted);font-size:14px}
</style></head>
<body><main><h1>$Title</h1><p>$Message</p></main></body></html>
"@
}

function Send-CallbackResponse {
    param([System.Net.HttpListenerContext]$Context, [int]$StatusCode, [string]$Title, [string]$Message)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes((Get-CallbackHtml -Title $Title -Message $Message))
    $response = $Context.Response
    $response.StatusCode = $StatusCode
    $response.ContentType = 'text/html; charset=utf-8'
    $response.ContentLength64 = $bytes.Length
    $response.OutputStream.Write($bytes, 0, $bytes.Length)
    $response.OutputStream.Close()
}

function Get-SpotifyErrorDetail {
    param($ErrorRecord)
    # StrictMode 3 下，$ErrorRecord.ErrorDetails 为 $null 时访问 .Message 会抛
    # “在此对象上找不到属性”，把 Spotify 的真实原因（invalid_grant 等）盖掉。
    $raw = $null
    $status = $null
    try {
        if ($null -ne $ErrorRecord) {
            $details = $ErrorRecord.ErrorDetails
            if ($null -ne $details -and $null -ne $details.Message) { $raw = [string]$details.Message }
            $response = $ErrorRecord.Exception.Response
            if ($null -ne $response -and $null -ne $response.StatusCode) { $status = [int]$response.StatusCode }
        }
    } catch { }
    if (-not $raw) {
        try { if ($null -ne $ErrorRecord -and $null -ne $ErrorRecord.Exception) { $raw = [string]$ErrorRecord.Exception.Message } } catch { }
    }
    if ($raw) {
        try {
            $obj = $raw | ConvertFrom-Json
            $parts = @()
            if ($null -ne $obj.PSObject.Properties['error'] -and $obj.error) { $parts += [string]$obj.error }
            if ($null -ne $obj.PSObject.Properties['error_description'] -and $obj.error_description) { $parts += [string]$obj.error_description }
            if ($parts.Count -gt 0) {
                $text = ($parts -join '：')
                if ($status) { $text = "$text（HTTP $status）" }
                return $text
            }
        } catch { }
        if ($status) { return "$raw（HTTP $status）" }
        return $raw
    }
    return '网络请求失败。请检查网络与系统时间是否准确，然后重新运行本工具。'
}

function Get-SpotifyToken {
    param([string]$ClientIdValue, [string]$RedirectUri, [string]$ScopeList, [int]$TimeoutSec)

    $redirect = [Uri]$RedirectUri
    $prefix = '{0}://{1}:{2}/' -f $redirect.Scheme, $redirect.Host, $redirect.Port
    $verifier = New-RandomHex -ByteCount 32
    $challenge = Get-Base64UrlSha256 -Text $verifier
    $state = New-RandomHex -ByteCount 16

    $listener = [System.Net.HttpListener]::new()
    $listener.Prefixes.Add($prefix)
    try {
        $listener.Start()
    } catch {
        $listener.Close()
        throw "无法在 $prefix 启动本地回调服务（端口可能被占用）。请关闭占用该端口的程序，或用 -Redirect 换成其它本地端口，并在 Spotify 应用设置里同步 Redirect URI。"
    }
    $listener.IgnoreWriteExceptions = $true

    $authUrl = 'https://accounts.spotify.com/authorize?' + (@(
            'client_id=' + [Uri]::EscapeDataString($ClientIdValue)
            'response_type=code'
            'redirect_uri=' + [Uri]::EscapeDataString($RedirectUri)
            'scope=' + [Uri]::EscapeDataString($ScopeList)
            'state=' + $state
            'code_challenge_method=S256'
            'code_challenge=' + $challenge
        ) -join '&')

    try {
        try {
            Start-Process $authUrl | Out-Null
            Write-Host '已在默认浏览器打开 Spotify 授权页面，请登录并点击“同意”。' -ForegroundColor Cyan
        } catch {
            Write-Host '无法自动打开浏览器。请手动在浏览器访问下面的授权地址：' -ForegroundColor Yellow
            Write-Host $authUrl
        }

        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
        while ($true) {
            $task = $listener.GetContextAsync()
            while (-not $task.AsyncWaitHandle.WaitOne(200)) {
                if ([DateTime]::UtcNow -ge $deadline) {
                    throw "等待浏览器授权超时（$TimeoutSec 秒）。请重新运行本工具再次授权。"
                }
            }
            $context = $task.GetAwaiter().GetResult()

            if ($context.Request.Url.AbsolutePath -ne $redirect.AbsolutePath) {
                Send-CallbackResponse -Context $context -StatusCode 404 -Title '页面不存在' -Message '这不是授权回调地址，可以关闭此页面。'
                continue
            }

            $query = $context.Request.QueryString
            if ($query['error']) {
                Send-CallbackResponse -Context $context -StatusCode 200 -Title '授权未完成' -Message '你在 Spotify 页面取消或未同意所需权限。请回到终端重新运行本工具。'
                throw "Spotify 授权未完成（$($query['error'])）。请重新运行本工具并同意所需权限。"
            }

            # state 必须与本次请求一致，防止回调被伪造。
            if ([string]$query['state'] -cne $state) {
                Send-CallbackResponse -Context $context -StatusCode 400 -Title '状态校验失败' -Message '回调中的 state 不匹配，已中止。请回到终端重新运行本工具。'
                throw '回调 state 校验失败，已中止。请重新运行本工具。'
            }

            $code = [string]$query['code']
            if (-not $code) {
                Send-CallbackResponse -Context $context -StatusCode 400 -Title '缺少授权码' -Message '回调中没有授权码。请回到终端重新运行本工具。'
                throw '回调缺少授权码，请重新运行本工具。'
            }

            Write-Host '已收到授权码，正在换取访问令牌…' -ForegroundColor Cyan
            try {
                # 注意：Invoke-RestMethod 没有 -AsHashtable（那是 ConvertFrom-Json 的），
                # 传了会直接报“找不到与参数名称 AsHashtable 匹配的参数”。这里返回 PSCustomObject。
                $tokenResponse = Invoke-RestMethod -Method Post -Uri 'https://accounts.spotify.com/api/token' `
                    -ContentType 'application/x-www-form-urlencoded' -TimeoutSec 30 -Body @{
                    grant_type    = 'authorization_code'
                    code          = $code
                    redirect_uri  = $RedirectUri
                    client_id     = $ClientIdValue
                    code_verifier = $verifier
                }
                $tokenMap = @{}
                if ($null -ne $tokenResponse) {
                    foreach ($prop in $tokenResponse.PSObject.Properties) { $tokenMap[$prop.Name] = $prop.Value }
                }
            } catch {
                $detail = Get-SpotifyErrorDetail -ErrorRecord $_
                Send-CallbackResponse -Context $context -StatusCode 200 -Title '授权未完成' -Message '令牌交换失败。请回到终端查看原因后重新运行本工具。'
                throw @"
用授权码换取令牌失败：$detail
请依次核对：
  1. Spotify 开发者后台该应用的 Redirect URI 是否与 $RedirectUri 完全一致（协议、127.0.0.1、端口、路径都要一样）；
  2. 授权码一次性且 60 秒内有效，重新运行本工具即可换新码；
  3. 应用是否为 Development 模式且当前账号在允许列表内（否则授权后仍会被拒）。
"@
            }

            if (-not $tokenMap['access_token']) {
                Send-CallbackResponse -Context $context -StatusCode 200 -Title '授权未完成' -Message '未拿到访问令牌。请回到终端重新运行本工具。'
                throw 'Spotify 未返回 access_token，请重新运行本工具。'
            }
            if (-not $tokenMap['refresh_token']) {
                Send-CallbackResponse -Context $context -StatusCode 200 -Title '授权未完成' -Message '未拿到刷新令牌。请回到终端重新运行本工具。'
                throw 'Spotify 未返回 refresh_token。请检查 Spotify 应用的授权设置后重新授权。'
            }

            Send-CallbackResponse -Context $context -StatusCode 200 -Title '授权成功' `
                -Message '声环已获得 Spotify 授权。请回到终端窗口继续，看到“配置完成”后即可关闭本页面。'

            $expiresAtMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + [long]$tokenMap['expires_in'] * 1000
            $scopeText = if ($tokenMap['scope']) { [string]$tokenMap['scope'] } else { $ScopeList }
            return [pscustomobject]@{
                AccessToken  = [string]$tokenMap['access_token']
                RefreshToken = [string]$tokenMap['refresh_token']
                Scope        = $scopeText
                ExpiresAtMs  = [long]$expiresAtMs
            }
        }
    } finally {
        $listener.Stop()
        $listener.Close()
    }
}

# ---------------------------------------------------------------- 自检

function Invoke-SelfTest {
    Write-Host ''
    Write-Host '声环 SoundRing 配置工具 · 本地自检（不联网、不连接设备）' -ForegroundColor White
    $checks = [System.Collections.Generic.List[object]]::new()

    # 1) PBKDF2-HMAC-SHA256 标准向量：PBKDF2("password", "salt", 1 次, 32 字节)
    $expected = '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b'
    $actual = ''
    $detail = ''
    try {
        $derived = [System.Security.Cryptography.Rfc2898DeriveBytes]::Pbkdf2(
            'password', [System.Text.Encoding]::UTF8.GetBytes('salt'), 1,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256, 32)
        $actual = [Convert]::ToHexString($derived).ToLowerInvariant()
    } catch {
        $detail = $_.Exception.Message
    }
    if ($actual -ne $expected) {
        if (-not $detail) { $detail = "期望 $expected，实际 $actual" }
    }
    $checks.Add([pscustomobject]@{ Name = 'PBKDF2-HMAC-SHA256 标准向量'; Ok = ($actual -eq $expected); Detail = $detail })

    # 2) AES-256-GCM 往返 + 篡改 tag 必须失败
    $key = [byte[]]::new(32)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($key)
    $plainText = '{"ssid":"声环测试","pass":"p@ss w0rd"}'
    $aad = 'sr1|wifi|1'
    $roundTripOk = $false
    $roundTripDetail = ''
    $tamperOk = $false
    $tamperDetail = ''
    try {
        $sealed = Protect-SrPayload -Key $key -Aad $aad -PlainText $plainText
        $back = Unprotect-SrPayload -Key $key -Aad $aad -IvHex $sealed.IvHex -CtHex $sealed.CtHex -TagHex $sealed.TagHex
        $roundTripOk = ($back -ceq $plainText)
        if (-not $roundTripOk) { $roundTripDetail = '解密结果与明文不一致' }

        $chars = $sealed.TagHex.ToCharArray()
        $last = $chars[$chars.Length - 1]
        $chars[$chars.Length - 1] = if ($last -eq '0') { '1' } else { '0' }
        $tamperedTag = -join $chars
        $threw = $false
        try {
            [void](Unprotect-SrPayload -Key $key -Aad $aad -IvHex $sealed.IvHex -CtHex $sealed.CtHex -TagHex $tamperedTag)
        } catch {
            $threw = $true
        }
        $tamperOk = $threw
        if (-not $threw) { $tamperDetail = '篡改 tag 后仍然解密成功' }
    } catch {
        $roundTripDetail = $_.Exception.Message
        $tamperDetail = $_.Exception.Message
    }
    $checks.Add([pscustomobject]@{ Name = 'AES-256-GCM 加解密往返'; Ok = $roundTripOk; Detail = $roundTripDetail })
    $checks.Add([pscustomobject]@{ Name = 'AES-256-GCM 篡改 tag 被拒绝'; Ok = $tamperOk; Detail = $tamperDetail })

    # 3) HMAC-SHA256("hello")：长度 32 字节且结果稳定
    $hmacOk = $false
    $hmacDetail = ''
    try {
        $first = Get-HmacSha256 -Key $key -Text 'hello'
        $second = Get-HmacSha256 -Key $key -Text 'hello'
        $hmacOk = ($first.Length -eq 32) -and ($second.Length -eq 32) -and
                  ([Convert]::ToHexString($first) -ceq [Convert]::ToHexString($second))
        if (-not $hmacOk) { $hmacDetail = "长度 $($first.Length) / 两次结果不一致" }
    } catch {
        $hmacDetail = $_.Exception.Message
    }
    $checks.Add([pscustomobject]@{ Name = 'HMAC-SHA256("hello") 长度与稳定性'; Ok = $hmacOk; Detail = $hmacDetail })

    Clear-Bytes $key

    Write-Host ''
    $failed = 0
    foreach ($check in $checks) {
        if ($check.Ok) {
            Write-Host ("[PASS] " + $check.Name) -ForegroundColor Green
        } else {
            $failed++
            $suffix = if ($check.Detail) { ' — ' + $check.Detail } else { '' }
            Write-Host ("[FAIL] " + $check.Name + $suffix) -ForegroundColor Red
        }
    }
    Write-Host ''
    if ($failed -eq 0) {
        Write-Host "自检全部通过（$($checks.Count)/$($checks.Count)）。" -ForegroundColor Green
        return $true
    }
    Write-Host "自检失败 $failed 项（共 $($checks.Count) 项）。" -ForegroundColor Red
    return $false
}

if ($SelfTest) {
    $selfTestPassed = Invoke-SelfTest
    if ($selfTestPassed) { exit 0 }
    exit 1
}

# ---------------------------------------------------------------- 主流程

$exitCode = 1
$serialPort = $null
$token = $null

try {
    if ($AuthOnly -and $WifiOnly) { throw '不能同时选择仅授权和仅配置 Wi-Fi。' }
    if (-not $WifiOnly) {
    if (-not $ClientId) { $ClientId = (Read-Host '请输入 Spotify 应用的 Client ID').Trim() }
    if ($ClientId -notmatch '^[0-9a-fA-F]{32}$') {
        throw '-ClientId 格式不正确，应为 32 位十六进制。请从 Spotify 开发者后台重新复制 Client ID。'
    }

    if (-not $Redirect) { $Redirect = (Read-Host '请输入 Spotify 应用中登记的本机回调地址').Trim() }
    $redirectUri = $null
    try { $redirectUri = [Uri]$Redirect } catch { $redirectUri = $null }
    if (-not $redirectUri -or -not $redirectUri.IsAbsoluteUri -or $redirectUri.Scheme -ne 'http' -or -not $redirectUri.IsLoopback) {
        throw "-Redirect 必须指向本机 http 地址，并与 Spotify 应用登记的回调地址完全一致。"
    }

    }

    if (-not $AuthOnly) {
        if (-not $Ssid) { $Ssid = Read-Host '请输入 Wi-Fi 名称（SSID）' }
        if (-not $Ssid) { throw '未提供 Wi-Fi 名称。' }
        if (-not $PSBoundParameters.ContainsKey('Password')) {
            $securePassword = Read-Host '请输入 Wi-Fi 密码（不回显；开放网络直接回车）' -AsSecureString
            $Password = [System.Net.NetworkCredential]::new('', $securePassword).Password
            $securePassword.Dispose()
        }
    }

    $portName = $Port
    if (-not $portName) { $portName = Find-PassportPort }
    if (-not $portName) {
        $available = @([System.IO.Ports.SerialPort]::GetPortNames())
        $suffix = if ($available.Count -gt 0) { "（当前可用串口：$($available -join '、')）" } else { '（当前没有可用串口）' }
        throw "未找到 VID_303A 的设备串口$suffix。请确认设备已用 USB 连接到电脑并已上电；如果已连接，可用 -Port 指定串口。"
    }

    Write-Host ''
    Write-Host "使用串口 $portName（115200 8N1）。" -ForegroundColor Cyan
    $serialPort = [System.IO.Ports.SerialPort]::new($portName, 115200, 'None', 8, 'One')
    $serialPort.ReadTimeout = 200
    $serialPort.WriteTimeout = 2000
    $serialPort.Encoding = [System.Text.Encoding]::UTF8
    try {
        $serialPort.Open()
    } catch {
        throw "无法打开串口 $portName：$($_.Exception.Message)。请关闭占用该串口的程序（如串口监视器、烧录工具），然后重试。"
    }
    $serialPort.DiscardInBuffer()

    Write-Host '请现在在设备上进入 设置 → 电脑配对（如果已经进入，请退出后重新进入，设备才会重新广播配对信息）。' -ForegroundColor Yellow
    Write-Host "正在等待设备广播配对信息（最长 $TimeoutSec 秒）…"
    $frame = Wait-DeviceLine -SerialPort $serialPort -Prefixes @('SRPAIR1') -TimeoutSec $TimeoutSec

    $pairInfo = $null
    try { $pairInfo = $frame.Json | ConvertFrom-Json -AsHashtable } catch { $pairInfo = $null }
    if (-not $pairInfo -or [int]$pairInfo['v'] -ne 1 -or
        [string]$pairInfo['nonce'] -notmatch '^[0-9a-fA-F]{32}$') {
        throw '收到的配对信息无法识别。请确认设备固件已更新，然后在设备上重新进入 设置 → 电脑配对。'
    }

    # 配对码只显示在设备屏幕上，不随串口广播；必须由使用者输入。
    $pairCode = ''
    while ($pairCode -notmatch '^\d{8}$') {
        $pairCode = Read-Host '请输入设备屏幕上显示的 8 位配对码'
        if ($pairCode -notmatch '^\d{8}$') {
            Write-Host '配对码应为 8 位数字，请重新输入。' -ForegroundColor Yellow
        }
    }
    Write-Host '已收到配对信息，正在派生密钥…' -ForegroundColor Green

    $nonceBytes = [Convert]::FromHexString([string]$pairInfo['nonce'])
    $script:SrKey = Get-SrKey -Code $pairCode -Nonce $nonceBytes
    Clear-Bytes $nonceBytes
    $pairCode = $null
    $pairInfo = $null

    Write-Host '[1/4] 正在与设备握手…'
    $mac = Get-HmacSha256 -Key $script:SrKey -Text 'hello'
    $helloLine = '{"v":1,"cmd":"hello","mac":"' + [Convert]::ToHexString($mac).ToLowerInvariant() + '"}'
    Send-DeviceLine -SerialPort $serialPort -Line $helloLine
    [void](Resolve-DeviceReply -Reply (Wait-DeviceLine -SerialPort $serialPort -Prefixes @('SROK', 'SRERR') -TimeoutSec 20) -Cmd 'hello')
    Write-Host '握手成功。' -ForegroundColor Green

    $authSequence = 1
    if (-not $AuthOnly) {
    Write-Host '[2/4] 正在写入 Wi-Fi 配置（设备可能需要十几秒连接路由器）…'
    $wifiJson = @{ ssid = $Ssid; pass = $Password } | ConvertTo-Json -Compress
    [void](Invoke-SecureCommand -SerialPort $serialPort -Key $script:SrKey -Cmd 'wifi' -Seq 1 -PlainJson $wifiJson -TimeoutSec 90)
    $wifiJson = $null
    Write-Host 'Wi-Fi 配置已写入，正在确认设备联网…'

    # 写入成功不等于连上：轮询设备状态，避免把“配置写进去了但没联网”当成成功。
    $wifiStatus = $null
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        Start-Sleep -Milliseconds 1500
        try {
            $wifiStatus = Get-DeviceStatus -SerialPort $serialPort
            if ($wifiStatus.Connected) { break }
        } catch { }
    }
    if ($wifiStatus -and $wifiStatus.Connected) {
        Write-Host "设备已联网（IP $($wifiStatus.Ip)）。" -ForegroundColor Green
    } else {
        throw @"
设备在 30 秒内没有连上 Wi-Fi（SSID: $Ssid）。
请检查：1) 该 Wi-Fi 是 2.4GHz（设备不支持 5GHz）；2) 密码正确（区分大小写）；3) 信号足够强。
设备仍会在后台按 3s→60s 退避自动重连；网络改好后可在设备上重新进入 设置 → 电脑配对，再运行本工具。
"@
    }

        $authSequence = 2
    } else {
        $wifiStatus = Get-DeviceStatus -SerialPort $serialPort
        if ($wifiStatus.Connected) { Write-Host '设备报告 Wi-Fi 已连接。' -ForegroundColor Green }
        else { Write-Host '设备报告 Wi-Fi 尚未连接；仍可写入授权，但播放控制需要网络。' -ForegroundColor Yellow }
    }

    if ($WifiOnly) {
        Send-DeviceLine -SerialPort $serialPort -Line '{"v":1,"cmd":"bye"}'
        try { [void](Wait-DeviceLine -SerialPort $serialPort -Prefixes @('SROK') -TimeoutSec 10) } catch { }
        Write-Host 'Wi-Fi 已连接并确认。' -ForegroundColor Green
        $exitCode = 0
        return
    }

    Write-Host '[3/4] 正在打开浏览器完成 Spotify 授权（设备配对 10 分钟后失效，请尽快完成）…'
    $token = Get-SpotifyToken -ClientIdValue $ClientId -RedirectUri $Redirect -ScopeList $Scopes -TimeoutSec $TimeoutSec
    Write-Host '已获得 Spotify 授权。' -ForegroundColor Green

    Write-Host '[4/4] 正在写入 Spotify 授权…'
    $authJson = @{
        client_id      = $ClientId
        access_token   = $token.AccessToken
        refresh_token  = $token.RefreshToken
        scope          = $token.Scope
        expires_at_ms  = [long]$token.ExpiresAtMs
    } | ConvertTo-Json -Compress
    [void](Invoke-SecureCommand -SerialPort $serialPort -Key $script:SrKey -Cmd 'auth' -Seq $authSequence -PlainJson $authJson -TimeoutSec 60)
    $authJson = $null
    $token = $null

    # 回读设备状态，确认令牌真的落盘（而不是只回了 SROK）。
    $authOk = $false
    for ($attempt = 0; $attempt -lt 6; $attempt++) {
        try {
            $st = Get-DeviceStatus -SerialPort $serialPort
            if ($st.AuthReady) { $authOk = $true; break }
        } catch { }
        Start-Sleep -Milliseconds 800
    }
    if (-not $authOk) {
        throw '设备没有确认保存授权（status 里 auth=0）。请重新运行本工具；若反复出现，请把这一行反馈给维护者。'
    }
    Write-Host 'Spotify 授权已写入并确认。' -ForegroundColor Green

    Send-DeviceLine -SerialPort $serialPort -Line '{"v":1,"cmd":"bye"}'
    try {
        [void](Wait-DeviceLine -SerialPort $serialPort -Prefixes @('SROK') -TimeoutSec 10)
    } catch {
        # 设备可能已直接结束会话，这里不作为失败。
    }

    Write-Host ''
    Write-Host '授权已保存。设备联网并完成校时后即可尝试 Spotify 控制。' -ForegroundColor Green
    Write-Host '如果设备仍提示未配置：请重新在设备上进入 设置 → 电脑配对，再运行本工具。'
    $exitCode = 0
} catch {
    Write-Host ''
    Write-Host "失败：$($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    if ($serialPort) {
        try { if ($serialPort.IsOpen) { $serialPort.Close() } } catch { }
        try { $serialPort.Dispose() } catch { }
        $serialPort = $null
    }
    if ($script:SrKey) {
        Clear-Bytes $script:SrKey
        $script:SrKey = $null
    }
    $token = $null
    $Password = $null
    $Ssid = $null
    $script:RxPending = ''
}

exit $exitCode
