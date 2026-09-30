# PLY(ascii) -> OBJ 转换：只取顶点位置的 x/y/z 与面的顶点索引，丢弃 confidence/intensity 等属性。
#
# 为什么要有这个脚本：Nanite 示例只吃 OBJ，而手上的数据集是 PLY；之前那次转换是一次性命令、
# 没落盘，结果 bunny_hi.obj 只剩 49644 面（PLY 里是 69451 面，全是三角形），少了 28.5% ——
# 模型上表现为破洞。这里把转换固定下来，可复现、可核对。
#
# 用法：  pwsh -File ply2obj.ps1 -Ply ..\..\BunnyPBR\Res\bunny_hi.ply -Obj ..\Res\bunny_hi.obj
#
# 要点（这几处正是容易出错的地方）：
#   · 流式逐行读，不用 ReadAllLines —— bunny_hi 之外的模型有几十 MB；
#   · 数值一律用 InvariantCulture 解析/格式化，不受系统区域影响；
#   · OBJ 索引是 **1 基**，PLY 是 0 基，必须 +1；
#   · 面按 PLY 的 list 长度解析（本数据集全是 3），长度 >3 时做扇形三角化；
#   · 输出 ASCII + '\n'，避免 CRLF/BOM 干扰 C 侧解析。
param(
    [Parameter(Mandatory = $true)][string]$Ply,
    [Parameter(Mandatory = $true)][string]$Obj
)
$ErrorActionPreference = "Stop"
$ci = [System.Globalization.CultureInfo]::InvariantCulture

$sr = New-Object System.IO.StreamReader($Ply, [System.Text.Encoding]::ASCII)
$vertexCount = 0
$faceCount = 0
while (($line = $sr.ReadLine()) -ne $null) {
    if ($line.StartsWith('element vertex ')) { $vertexCount = [int]$line.Substring(15).Trim() }
    elseif ($line.StartsWith('element face ')) { $faceCount = [int]$line.Substring(13).Trim() }
    elseif ($line.StartsWith('end_header')) { break }
}
if ($vertexCount -le 0 -or $faceCount -le 0) {
    $sr.Close()
    throw "PLY header parse failed (vertex=$vertexCount face=$faceCount): $Ply"
}

$sw = New-Object System.IO.StreamWriter($Obj, $false, [System.Text.Encoding]::ASCII)
$sw.NewLine = "`n"

$sep = [char[]]" `t"
$vn = 0
for ($i = 0; $i -lt $vertexCount; $i++) {
    $l = $sr.ReadLine()
    if ($l -eq $null) { break }
    $t = $l.Split($sep, [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($t.Count -lt 3) { continue }
    $x = [float]::Parse($t[0], $ci).ToString('R', $ci)
    $y = [float]::Parse($t[1], $ci).ToString('R', $ci)
    $z = [float]::Parse($t[2], $ci).ToString('R', $ci)
    $sw.WriteLine("v $x $y $z")
    $vn++
}

$tri = 0
for ($i = 0; $i -lt $faceCount; $i++) {
    $l = $sr.ReadLine()
    if ($l -eq $null) { break }
    $t = $l.Split($sep, [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($t.Count -lt 4) { continue }
    $n = [int]$t[0]
    if ($n -lt 3 -or $t.Count -lt ($n + 1)) { continue }
    for ($k = 1; $k -le ($n - 2); $k++) {
        $a = [int]$t[1] + 1
        $b = [int]$t[$k + 1] + 1
        $c = [int]$t[$k + 2] + 1
        $sw.WriteLine("f $a $b $c")
        $tri++
    }
}

$sw.Flush(); $sw.Close(); $sr.Close()
Write-Host ("{0} -> {1} : v={2} tri={3} (PLY 头声明 v={4} face={5})" -f `
    (Split-Path $Ply -Leaf), (Split-Path $Obj -Leaf), $vn, $tri, $vertexCount, $faceCount)
