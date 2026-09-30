<#
.SYNOPSIS
    把打包好的 APK 装到设备上跑起来，并抓 logcat + 截图（Android 侧的验证工具）。

.DESCRIPTION
    与 package_android_apk.ps1 的分工：那个负责「构建产物 → 签名 APK」，
    这个负责「装到手机 → 启动 → 抓日志/截图 → 停掉」。

.EXAMPLE
    pwsh tools/run_android.ps1 -Apk build-android\apk\TextureCube-arm64-v8a.apk -Seconds 12
#>
[CmdletBinding()]
param(
    [string] $Apk      = "build-android\apk\TextureCube-arm64-v8a.apk",
    [string] $SdkDir   = "C:\Program Files (x86)\Android\android-sdk",
    [string] $Package  = "com.fisir.texturecube",
    [string] $Activity = "android.app.NativeActivity",
    [int]    $Seconds  = 12,
    [string] $ShotPath = "",
    [switch] $KeepRunning
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$adb = Join-Path $SdkDir "platform-tools\adb.exe"
if (-not (Test-Path $adb)) { throw "找不到 adb：$adb" }
$Apk = (Resolve-Path $Apk).Path

# 只挑「已授权」的设备（offline / unauthorized 的会被忽略）
$devices = @()
foreach ($line in (& $adb devices)) {
    if ($line -match '^(\S+)\s+device\b') { $devices += $Matches[1] }
}
if (-not $devices) { throw "没有已授权的设备：插上手机、开 USB 调试、在手机上点『允许』" }
$serial = $devices[0]
if ($devices.Count -gt 1) { Write-Host "  多台设备，用第一台：$serial" -ForegroundColor Yellow }
$adbArgs = @("-s", $serial)
Write-Host "设备: $serial" -ForegroundColor Cyan

Write-Host "=== 安装 ===" -ForegroundColor Cyan
& $adb @adbArgs install -r $Apk
if ($LASTEXITCODE -ne 0) { throw "adb install 失败（exit=$LASTEXITCODE）" }

Write-Host "=== 清空 logcat 并启动 ===" -ForegroundColor Cyan
& $adb @adbArgs logcat -c
& $adb @adbArgs shell am start -n "$Package/$Activity" | Out-Host
Start-Sleep -Seconds $Seconds

if ($ShotPath -eq "") { $ShotPath = Join-Path $repoRoot "build-android\apk\device-screenshot.png" }
Write-Host "=== 截图 → $ShotPath ===" -ForegroundColor Cyan
$shotDir = Split-Path -Parent $ShotPath
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$bytes = & $adb @adbArgs exec-out screencap -p
[System.IO.File]::WriteAllBytes($ShotPath, $bytes)

Write-Host "=== logcat（FISIR / Vulkan / 崩溃相关）===" -ForegroundColor Cyan
& $adb @adbArgs logcat -d -v time |
    Select-String -Pattern "FISIR|Vulkan|vulkan|TextureCube|AndroidRuntime|DEBUG|libc|stdout|stderr|Adreno|adreno" |
    Select-Object -Last 60 |
    ForEach-Object { $_.Line }

Write-Host "=== 进程状态 ===" -ForegroundColor Cyan
& $adb @adbArgs shell "ps -A | grep -i texturecube" | Out-Host

if (-not $KeepRunning) {
    Write-Host "=== 停掉应用 ===" -ForegroundColor Cyan
    & $adb @adbArgs shell am force-stop $Package
}
Write-Host "完成。截图：$ShotPath" -ForegroundColor Green
