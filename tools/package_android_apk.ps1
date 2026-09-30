<#
.SYNOPSIS
    把 CMake 交叉编译出来的 Android 产物打成一个可安装的 APK（不依赖 Gradle）。

.DESCRIPTION
    流程：aapt2 link（清单 + assets）→ 把 .so 塞进 lib/<abi>/ → zipalign → apksigner 签名 → 可选 adb 安装。
    全部用 Android SDK 自带的命令行工具 + .NET 的 ZipFile，所以不需要 Gradle/AGP，
    也不会往工程里引第三方的 Gradle 配置。

    应用是纯原生的（android:hasCode="false" + android.app.NativeActivity），因此没有 dex：
    系统加载 libTextureCube.so 后由 native_app_glue 调 android_main。

.EXAMPLE
    pwsh tools/package_android_apk.ps1 -BuildDir build-android -Install
#>
[CmdletBinding()]
param(
    [string] $BuildDir   = "build-android",
    [string] $Abi        = "arm64-v8a",
    [string] $SdkDir     = "C:\Program Files (x86)\Android\android-sdk",
    [string] $NdkDir     = "C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r27c",
    [string] $SlangDir   = "vendor\slang-android-arm64\lib",
    [string] $AppLibName = "libTextureCube.so",
    [string] $ApiLevel   = "36",
    [switch] $Install
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot          # tools/ 的上一级 = 仓库根
if (-not (Test-Path (Join-Path $repoRoot "CMakeLists.txt"))) { throw "找不到仓库根（$repoRoot）" }
Set-Location $repoRoot

function Find-Newest([string] $dir, [string] $filter) {
    if (-not (Test-Path $dir)) { return $null }
    Get-ChildItem $dir -Filter $filter -File -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
}
function Assert-Path([string] $path, [string] $what) {
    if (-not (Test-Path $path)) { throw "缺少$what：$path" }
    return (Resolve-Path $path).Path
}

Write-Host "=== 1/6 定位工具链 ===" -ForegroundColor Cyan
$buildTools = Find-Newest (Join-Path $SdkDir "build-tools") "36.0.0"
if (-not $buildTools) { $buildTools = Get-ChildItem (Join-Path $SdkDir "build-tools") -Directory | Sort-Object Name -Descending | Select-Object -First 1 }
$aapt2     = Assert-Path (Join-Path $buildTools.FullName "aapt2.exe")     "aapt2"
$zipalign  = Assert-Path (Join-Path $buildTools.FullName "zipalign.exe")  "zipalign"
$apksigner = Assert-Path (Join-Path $buildTools.FullName "apksigner.bat") "apksigner"
$adb       = Join-Path $SdkDir "platform-tools\adb.exe"
$androidJar = Assert-Path (Join-Path $SdkDir "platforms\android-$ApiLevel\android.jar") "android.jar"
Write-Host "  build-tools : $($buildTools.Name)"
Write-Host "  android.jar : $androidJar"

Write-Host "=== 2/6 收集产物 ===" -ForegroundColor Cyan
$built = @{}
foreach ($name in @($AppLibName, "libRHIVK.so", "libLog.so")) {
    $hit = Get-ChildItem $BuildDir -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $hit) { throw "在 $BuildDir 里找不到 $name（先跑交叉编译：cmake --build build-android）" }
    $built[$name] = $hit.FullName
    Write-Host ("  {0,-22} {1}" -f $name, $hit.FullName)
}

# Slang 运行时（Android 版交叉编译产物）。只需要装好的那几个：libslang-compiler.so 是链接期
# NEEDED 的那个，glslang / glsl-module / rt 由它运行期 dlopen（名字里带版本号，别漏）。
# 先在 -SlangDir（装好的 SDK）里找；找不到再退回源码树的 Release/lib。
$slangLibs = @()
foreach ($dir in @($SlangDir)) {
    if (Test-Path $dir) {
        $slangLibs += Get-ChildItem $dir -Filter "libslang*.so" -File -ErrorAction SilentlyContinue
    }
}
if ($slangLibs.Count -eq 0) {
    $fallback = "vendor\slang-source\build-android-arm64-v8a\Release\lib"
    if (Test-Path $fallback) {
        $slangLibs += Get-ChildItem $fallback -Filter "libslang*.so" -File -ErrorAction SilentlyContinue
    }
}
# 按文件名去重（同名只留一个，避免把中间产物目录里不该进 APK 的库也塞进去）
$seen = @{}
$slangLibs = $slangLibs | Where-Object { -not $seen.ContainsKey($_.Name) -and ($seen[$_.Name] = $true) }
if ($slangLibs.Count -eq 0) {
    throw "找不到 Android 版 Slang 运行库（-SlangDir=$SlangDir）。着色器是运行期编译的，缺它必崩。"
}
foreach ($l in $slangLibs) { Write-Host ("  {0,-28} {1}" -f $l.Name, $l.FullName) }

# libc++_shared.so（NDK 提供；多个 .so 共用一份 STL，所以统一用 c++_shared）
$stlCandidates = @(
    (Join-Path $NdkDir "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"),
    (Join-Path $NdkDir "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\x86_64-linux-android\libc++_shared.so")
)
$stlSrc = $stlCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $stlSrc) { throw "在 NDK 里找不到 libc++_shared.so（$NdkDir）" }
Write-Host ("  {0,-28} {1}" -f "libc++_shared.so", $stlSrc)

Write-Host "=== 3/6 准备 assets 与清单 ===" -ForegroundColor Cyan
$apkWork = Join-Path $BuildDir "apk"
$assetsDir = Join-Path $apkWork "assets"
New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
Copy-Item "Example\TextureCube\Test3.png" (Join-Path $assetsDir "Test3.png") -Force
$manifest = Assert-Path "tools\android\AndroidManifest.xml" "AndroidManifest.xml"
$baseApk = Join-Path $apkWork "base.apk"
if (Test-Path $baseApk) { Remove-Item $baseApk -Force }

Write-Host "=== 4/6 aapt2 link（清单 + assets）===" -ForegroundColor Cyan
& $aapt2 link -o $baseApk --manifest $manifest -I $androidJar -A $assetsDir `
    --min-sdk-version 31 --target-sdk-version $ApiLevel --version-code 1 --version-name "1.0"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link 失败（exit=$LASTEXITCODE）" }
Write-Host "  生成 $baseApk"

Write-Host "=== 5/6 塞入 native 库（lib/$Abi/）===" -ForegroundColor Cyan
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$toPack = @()
$toPack += @{ path = $built[$AppLibName]; entry = "lib/$Abi/$AppLibName" }
$toPack += @{ path = $built["libRHIVK.so"]; entry = "lib/$Abi/libRHIVK.so" }
$toPack += @{ path = $built["libLog.so"];   entry = "lib/$Abi/libLog.so" }
foreach ($l in $slangLibs) { $toPack += @{ path = $l.FullName; entry = "lib/$Abi/$($l.Name)" } }
$toPack += @{ path = $stlSrc; entry = "lib/$Abi/libc++_shared.so" }

$zip = [System.IO.Compression.ZipFile]::Open($baseApk, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($item in $toPack) {
        $entryName = $item.entry
        $existing = $zip.GetEntry($entryName)
        if ($existing) { $existing.Delete() }
        $entry = $zip.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::NoCompression)
        $out = $entry.Open()
        try {
            $bytes = [System.IO.File]::ReadAllBytes($item.path)
            $out.Write($bytes, 0, $bytes.Length)
        } finally { $out.Dispose() }
        Write-Host ("  + {0}" -f $entryName)
    }
} finally { $zip.Dispose() }

$alignedApk = Join-Path $apkWork "app-aligned.apk"
if (Test-Path $alignedApk) { Remove-Item $alignedApk -Force }
& $zipalign -f -p 4 $baseApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "zipalign 失败（exit=$LASTEXITCODE）" }

# 调试签名（首次运行时用 keytool 生成，口令固定为 android/android，与 Android Studio 的调试密钥同级用途）
$keystore = Join-Path $PSScriptRoot "android\debug.keystore"
if (-not (Test-Path $keystore)) {
    Write-Host "  生成调试密钥库 $keystore"
    $keytool = (Get-Command keytool -ErrorAction SilentlyContinue).Source
    if (-not $keytool) {
        $keytool = Get-ChildItem "$env:ProgramFiles\Java","$env:ProgramFiles\Common Files\Oracle\Java" -Recurse -Filter keytool.exe -ErrorAction SilentlyContinue |
                   Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $keytool) { throw "找不到 keytool（装 JDK，或用 Android Studio 生成调试密钥）" }
    & $keytool -genkeypair -keystore $keystore -storepass android -keypass android -alias androiddebugkey `
        -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
    if ($LASTEXITCODE -ne 0) { throw "keytool 生成密钥失败（exit=$LASTEXITCODE）" }
}

$signedApk = Join-Path $apkWork "TextureCube-$Abi.apk"
if (Test-Path $signedApk) { Remove-Item $signedApk -Force }
& $apksigner sign --ks $keystore --ks-pass pass:android --key-pass pass:android --out $signedApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "apksigner 签名失败（exit=$LASTEXITCODE）" }

Write-Host "=== 6/6 结果 ===" -ForegroundColor Green
Write-Host "  APK: $signedApk  ($([Math]::Round((Get-Item $signedApk).Length/1MB,2)) MB)"

if ($Install) {
    if (-not (Test-Path $adb)) { throw "找不到 adb：$adb" }
    $devices = & $adb devices | Select-String -Pattern "\tdevice$"
    if (-not $devices) { throw "没有已授权的设备（先插上手机、开 USB 调试、在手机上允许这台电脑）" }
    Write-Host "  安装到设备…" -ForegroundColor Cyan
    & $adb install -r $signedApk
    if ($LASTEXITCODE -ne 0) { throw "adb install 失败（exit=$LASTEXITCODE）" }
    Write-Host "  启动 Activity 并抓 30 行 logcat…" -ForegroundColor Cyan
    & $adb shell am start -n "com.fisir.texturecube/android.app.NativeActivity"
    Start-Sleep -Seconds 3
    & $adb logcat -d -t 60 | Select-String -Pattern "FISIR|TextureCube|Vulkan|AndroidRuntime|DEBUG|ERROR" | Select-Object -Last 30
}
