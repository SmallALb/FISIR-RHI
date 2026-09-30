<#
.SYNOPSIS
    把交叉编译出来的 Android 产物打成一个可安装的 APK（不依赖 Gradle）。

.DESCRIPTION
    流程：定位工具链 → 收集 .so → aapt2 link（清单 + assets）→ 把 .so 塞进 lib/<abi>/（不压缩）
          → zipalign 对齐 → apksigner 签名 → 自检并打印结果。

    需要收集的 .so 一共三类：
      ① 你自己的：libTextureCube.so（入口，名字必须等于清单里的 android.app.lib_name）、
         libRHIVK.so（RHICreator 运行期 dlopen）、libLog.so
      ② Slang 运行时（-SlangDir，默认 vendor\slang-android-arm64\lib）：libslang-compiler.so 等，
         着色器是运行期在手机上编译的，缺了必崩
      ③ NDK 的 libc++_shared.so（所有 C++ 库共用一份 STL）

    不传 -BuildDir 时自动探测：优先 out\build\*Android*（VS 的 CMake 配置，最新优先），
    其次 out\build\*，最后 build-android（命令行交叉编译）。APK 默认输出到 out\apk\。

    应用是纯原生的（android:hasCode="false" + android.app.NativeActivity），所以包里没有 dex。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/package_android_apk.ps1
    powershell -ExecutionPolicy Bypass -File tools/package_android_apk.ps1 -ApkName TextureCube-release.apk
    powershell -ExecutionPolicy Bypass -File tools/package_android_apk.ps1 -BuildDir build-android -Install
#>
[CmdletBinding()]
param(
    [string] $BuildDir   = "",                              # 空 = 自动探测
    [string] $OutDir     = "out\apk",                       # APK 输出目录
    [string] $Abi        = "arm64-v8a",
    [string] $SdkDir     = "",                              # 空 = 环境变量或默认路径
    [string] $NdkDir     = "",                              # 空 = 环境变量或默认路径
    [string] $SlangDir   = "vendor\slang-android-arm64\lib",
    [string] $AppLibName = "libTextureCube.so",
    [string] $ApiLevel   = "36",
    [string] $ApkName    = "",                              # 空 = TextureCube-<abi>.apk
    [switch] $Install
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot          # tools/ 的上一级 = 仓库根
if (-not (Test-Path (Join-Path $repoRoot "CMakeLists.txt"))) { throw "找不到仓库根（$repoRoot）" }
Set-Location $repoRoot

function Write-Step([string] $text) { Write-Host "=== $text ===" -ForegroundColor Cyan }
function Assert-Path([string] $path, [string] $what) {
    if (-not (Test-Path $path)) { throw "缺少$what：$path" }
    return (Resolve-Path $path).Path
}
# 取某个目录下最新的匹配文件（构建目录里常有中间产物副本，取最新的一般就是最终产物）
function Find-Newest([string] $dir, [string] $filter) {
    if (-not (Test-Path $dir)) { return $null }
    Get-ChildItem $dir -Filter $filter -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
# 优先用规范路径（编译产物的最终位置），找不到再全目录里找最新的
function Find-Lib([string] $buildDir, [string] $name, [string[]] $hints) {
    foreach ($h in $hints) {
        $p = Join-Path $buildDir $h
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    $hit = Find-Newest $buildDir $name
    if ($hit) { return $hit.FullName }
    return $null
}

# ── 0/6 准备：工具链与构建目录 ────────────────────────────────────────────────
Write-Step "0/6 定位工具链与构建目录"

if (-not $SdkDir) {
    foreach ($k in @("ANDROID_HOME", "ANDROID_SDK_ROOT")) {
        $v = [Environment]::GetEnvironmentVariable($k); if (-not $SdkDir -and $v -and (Test-Path $v)) { $SdkDir = $v }
    }
}
if (-not $SdkDir) { $SdkDir = "C:\Program Files (x86)\Android\android-sdk" }
if (-not (Test-Path $SdkDir)) { throw "找不到 Android SDK（用 -SdkDir 指定，或设 ANDROID_HOME）" }

if (-not $NdkDir) {
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { $NdkDir = $env:ANDROID_NDK_HOME }
}
if (-not $NdkDir) {
    $ndkRoots = @("C:\Program Files (x86)\Android\AndroidNDK", "C:\Program Files\Android\AndroidNDK")
    foreach ($r in $ndkRoots) {
        if (-not (Test-Path $r)) { continue }
        $newest = Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -like "android-ndk-*" } | Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { $NdkDir = $newest.FullName; break }
    }
}
if (-not $NdkDir -or -not (Test-Path $NdkDir)) { throw "找不到 Android NDK（用 -NdkDir 指定，或设 ANDROID_NDK_HOME）" }

$btRoot = Join-Path $SdkDir "build-tools"
$buildTools = Find-Newest $btRoot "36.0.0"
if (-not $buildTools) {
    $buildTools = Get-ChildItem $btRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
}
if (-not $buildTools) { throw "找不到 build-tools（$btRoot）" }
$aapt2     = Assert-Path (Join-Path $buildTools.FullName "aapt2.exe")     "aapt2"
$zipalign  = Assert-Path (Join-Path $buildTools.FullName "zipalign.exe")  "zipalign"
$apksigner = Assert-Path (Join-Path $buildTools.FullName "apksigner.bat") "apksigner"
$adb       = Join-Path $SdkDir "platform-tools\adb.exe"

$jar = Join-Path $SdkDir "platforms\android-$ApiLevel\android.jar"
if (-not (Test-Path $jar)) {
    $plat = Get-ChildItem (Join-Path $SdkDir "platforms") -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
    if (-not $plat) { throw "找不到 platforms\android-*\android.jar（SDK 里没装 platform）" }
    $jar = Join-Path $plat.FullName "android.jar"
    Write-Host "  （没找到 android-$ApiLevel，改用 $($plat.Name)）"
}
Write-Host "  SDK         : $SdkDir"
Write-Host "  NDK         : $NdkDir"
Write-Host "  build-tools : $($buildTools.Name)"
Write-Host "  android.jar : $jar"

# 自动探测构建目录：能同时找到三个必需 .so 的才算
if (-not $BuildDir) {
    $required = @($AppLibName, "libRHIVK.so", "libLog.so")
    $cands = @()
    if (Test-Path "out\build") {
        $cands += Get-ChildItem "out\build" -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match "Android" } | Sort-Object LastWriteTime -Descending
        $cands += Get-ChildItem "out\build" -Directory -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime -Descending
    }
    if (Test-Path "build-android") { $cands += Get-Item "build-android" }
    foreach ($c in $cands) {
        $ok = $true
        foreach ($n in $required) {
            if (-not (Get-ChildItem $c.FullName -Recurse -Filter $n -File -ErrorAction SilentlyContinue)) { $ok = $false; break }
        }
        if ($ok) { $BuildDir = $c.FullName; break }
    }
    if (-not $BuildDir) {
        throw "没找到含 [$($required -join ', ')] 的构建目录。先在 VS 里用 Android-ARM64-* 配置生成，或跑 cmake --build build-android；也可以用 -BuildDir 指定。"
    }
    Write-Host "  自动探测到构建目录：$BuildDir" -ForegroundColor Green
}
$BuildDir = (Resolve-Path $BuildDir).Path

# ── 1/6 收集原生库 ────────────────────────────────────────────────────────────
Write-Step "1/6 收集原生库"

$toPack = New-Object System.Collections.Generic.List[object]
$hints = @{
    $AppLibName    = @("Example\TextureCube\$AppLibName", "Example\TextureCube\$Abi\$AppLibName")
    "libRHIVK.so"  = @("Vulkan\libRHIVK.so", "Vulkan\$Abi\libRHIVK.so")
    "libLog.so"    = @("Log\libLog.so", "Log\$Abi\libLog.so")
}
foreach ($name in @($AppLibName, "libRHIVK.so", "libLog.so")) {
    $path = Find-Lib $BuildDir $name $hints[$name]
    if (-not $path) { throw "在 $BuildDir 里找不到 $name（先交叉编译）" }
    $toPack.Add(@{ path = $path; entry = "lib/$Abi/$name" })
    Write-Host ("  {0,-24} {1,7:N2} MB  {2}" -f $name, ((Get-Item $path).Length/1MB), $path)
}

# Slang 运行时：libslang-compiler.so 是链接期 NEEDED 的，glslang / gl-module / rt 由它运行期 dlopen
$slangLibs = @()
if (Test-Path $SlangDir) {
    $slangLibs += Get-ChildItem $SlangDir -Filter "libslang*.so" -File -ErrorAction SilentlyContinue
}
if ($slangLibs.Count -eq 0) {
    $fallback = "vendor\slang-source\build-android-arm64-v8a\Release\lib"
    if (Test-Path $fallback) {
        $slangLibs += Get-ChildItem $fallback -Filter "libslang*.so" -File -ErrorAction SilentlyContinue
    }
}
$seen = @{}
$slangLibs = $slangLibs | Where-Object { -not $seen.ContainsKey($_.Name) -and ($seen[$_.Name] = $true) }
if ($slangLibs.Count -eq 0) {
    throw "找不到 Android 版 Slang 运行库（-SlangDir=$SlangDir）。着色器是运行期在手机上编译的，缺它必崩；先在 vendor\slang-android-arm64 备好。"
}
foreach ($l in $slangLibs) {
    $toPack.Add(@{ path = $l.FullName; entry = "lib/$Abi/$($l.Name)" })
    Write-Host ("  {0,-24} {1,7:N2} MB  {2}" -f $l.Name, ($l.Length/1MB), $l.FullName)
}

$stlCandidates = @(
    (Join-Path $NdkDir "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"),
    (Join-Path $NdkDir "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\x86_64-linux-android\libc++_shared.so")
)
$stlSrc = $stlCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $stlSrc) { throw "在 NDK 里找不到 libc++_shared.so（$NdkDir）" }
$toPack.Add(@{ path = $stlSrc; entry = "lib/$Abi/libc++_shared.so" })
Write-Host ("  {0,-24} {1,7:N2} MB  {2}" -f "libc++_shared.so", ((Get-Item $stlSrc).Length/1MB), $stlSrc)

# ── 2/6 准备输出目录、assets 与清单 ───────────────────────────────────────────
Write-Step "2/6 准备 assets 与清单"

$work = Join-Path $OutDir "work"
$assetsDir = Join-Path $work "assets"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Android 上读不到工程目录，资源必须打进 assets（app 走 AAssetManager，见 Platform::LoadAsset）
$png = Assert-Path "Example\TextureCube\Test3.png" "Test3.png"
Copy-Item $png (Join-Path $assetsDir "Test3.png") -Force
Write-Host "  assets: Test3.png"
$manifest = Assert-Path "tools\android\AndroidManifest.xml" "AndroidManifest.xml"
$manifestText = Get-Content $manifest -Raw
if ($manifestText -notmatch [regex]::Escape($AppLibName.Replace("lib", "").Replace(".so", ""))) {
    Write-Host "  警告：清单里的 android.app.lib_name 可能和 $AppLibName 不一致" -ForegroundColor Yellow
}

$baseApk = Join-Path $work "base.apk"

# ── 3/6 aapt2 link（清单 + assets，无 dex）───────────────────────────────────
Write-Step "3/6 aapt2 link（清单 + assets）"
& $aapt2 link -o $baseApk --manifest $manifest -I $jar -A $assetsDir `
    --min-sdk-version 31 --target-sdk-version $ApiLevel --version-code 1 --version-name "1.0"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link 失败（exit=$LASTEXITCODE）" }
Write-Host "  生成 base.apk（清单 + resources.arsc + assets，无 dex）"

# ── 4/6 塞入 native 库 ───────────────────────────────────────────────────────
Write-Step "4/6 塞入 native 库 lib/$Abi/"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($baseApk, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($item in $toPack) {
        $existing = $zip.GetEntry($item.entry)
        if ($existing) { $existing.Delete() }
        # 必须不压缩：原生库要能直接映射，且 zipalign 按 4 字节页对齐才有效
        $entry = $zip.CreateEntry($item.entry, [System.IO.Compression.CompressionLevel]::NoCompression)
        $out = $entry.Open()
        try {
            $bytes = [System.IO.File]::ReadAllBytes($item.path)
            $out.Write($bytes, 0, $bytes.Length)
        } finally { $out.Dispose() }
        Write-Host "  + $($item.entry)"
    }
} finally { $zip.Dispose() }

# ── 5/6 对齐 + 签名 ──────────────────────────────────────────────────────────
Write-Step "5/6 zipalign + apksigner"
$alignedApk = Join-Path $work "app-aligned.apk"
& $zipalign -f -p 4 $baseApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "zipalign 失败（exit=$LASTEXITCODE）" }

$keystore = Join-Path $PSScriptRoot "android\debug.keystore"
if (-not (Test-Path $keystore)) {
    Write-Host "  生成调试密钥库 $keystore"
    $keytool = (Get-Command keytool -ErrorAction SilentlyContinue).Source
    if (-not $keytool) {
        $keytool = Get-ChildItem "$env:ProgramFiles\Java", "$env:ProgramFiles\Common Files\Oracle\Java" -Recurse -Filter keytool.exe -ErrorAction SilentlyContinue |
                   Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $keytool) { throw "找不到 keytool（装 JDK，或用 Android Studio 生成调试密钥）" }
    & $keytool -genkeypair -keystore $keystore -storepass android -keypass android -alias androiddebugkey `
        -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
    if ($LASTEXITCODE -ne 0) { throw "keytool 生成密钥失败（exit=$LASTEXITCODE）" }
}

if (-not $ApkName) { $ApkName = "TextureCube-$Abi.apk" }
$signedApk = Join-Path $OutDir $ApkName
if (Test-Path $signedApk) { Remove-Item $signedApk -Force }
& $apksigner sign --ks $keystore --ks-pass pass:android --key-pass pass:android --out $signedApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "apksigner 签名失败（exit=$LASTEXITCODE）" }

# ── 6/6 自检 + 结果 ─────────────────────────────────────────────────────────
Write-Step "6/6 自检"
& $apksigner verify --print-certs $signedApk 2>&1 | Select-Object -First 3 | ForEach-Object { "  $_" }
& $aapt2 dump badging $signedApk 2>&1 |
    Select-String -Pattern "^package:|^sdkVersion:|^minSdkVersion:|^targetSdkVersion:|^native-code:|^launchable-activity:" |
    ForEach-Object { "  $($_.Line)" }

$sizeMb = [Math]::Round((Get-Item $signedApk).Length / 1MB, 2)
$libCount = ($toPack | Measure-Object).Count
Write-Host ""
Write-Host "完成：$signedApk  ($sizeMb MB，$libCount 个原生库，ABI=$Abi)" -ForegroundColor Green
Write-Host "安装：adb install -r `"$signedApk`"   （或把 APK 拷到手机/模拟器上直接点开安装）"

if ($Install) {
    if (-not (Test-Path $adb)) { throw "找不到 adb：$adb" }
    $devices = & $adb devices | Select-String -Pattern "\tdevice$"
    if (-not $devices) { throw "没有已授权的设备（插上手机、开 USB 调试、允许这台电脑）" }
    Write-Host "  安装到设备…" -ForegroundColor Cyan
    & $adb install -r $signedApk
    if ($LASTEXITCODE -ne 0) { throw "adb install 失败（exit=$LASTEXITCODE）" }
    & $adb shell am start -n "com.fisir.texturecube/android.app.NativeActivity"
    Start-Sleep -Seconds 3
    & $adb logcat -d -t 60 | Select-String -Pattern "FISIR|TextureCube|Vulkan|AndroidRuntime|DEBUG|ERROR" | Select-Object -Last 30
}
