# pack_exe.ps1 —— 构建后处理：写入防篡改 SHA-256 槽 + 把 exe 填充到指定大小
# 用法: powershell -File pack_exe.ps1 -Exe <路径> -TargetMB 40
# 约定(必须与 MioIntegrity.h 完全一致):
#   标记 32 字节 = "MIO-TAMPER-CHECK" + 16 字节二进制特征
#   紧随其后 32 字节 = SHA-256 槽
#   校验范围 = [0, PE映像末尾) 但跳过这 64 字节标记块
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [int]$TargetMB = 40
)
$ErrorActionPreference = 'Stop'

$MagicTail  = [byte[]](0x9E,0x3B,0x7F,0x21,0xC4,0x58,0xA6,0x0D,0x11,0xF2,0x84,0x4B,0x6C,0xE5,0x97,0x30)
$MarkerSize = 64
$HashOff    = 32

if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 $Exe" }

$bytes = [System.IO.File]::ReadAllBytes($Exe)
Write-Host ("[pack] 输入 {0}  {1:N0} 字节" -f (Split-Path $Exe -Leaf), $bytes.Length)

# ---------- 1) 定位标记 ----------
$latin = [System.Text.Encoding]::GetEncoding(28591)
$s = $latin.GetString($bytes)
$off = -1
$from = 0
while ($true) {
    $i = $s.IndexOf("MIO-TAMPER-CHECK", $from)
    if ($i -lt 0) { break }
    $ok = $true
    for ($j = 0; $j -lt 16; $j++) { if ($bytes[$i + 16 + $j] -ne $MagicTail[$j]) { $ok = $false; break } }
    if ($ok) { $off = $i; break }
    $from = $i + 1
}
if ($off -lt 0) { throw "找不到防篡改标记 —— MioIntegrity.h 没编进去？" }
Write-Host ("[pack] 标记位于文件偏移 0x{0:X}" -f $off)

# ---------- 2) 算 PE 映像末尾 ----------
$e_lfanew = [BitConverter]::ToUInt32($bytes, 0x3C)
if ([BitConverter]::ToUInt32($bytes, $e_lfanew) -ne 0x00004550) { throw "不是有效 PE" }
$nsec   = [BitConverter]::ToUInt16($bytes, $e_lfanew + 6)
$optsz  = [BitConverter]::ToUInt16($bytes, $e_lfanew + 20)
$secoff = $e_lfanew + 24 + $optsz
$end = [int64]$secoff + [int64]$nsec * 40
for ($i = 0; $i -lt $nsec; $i++) {
    $o = $secoff + $i * 40
    $rawsz  = [BitConverter]::ToUInt32($bytes, $o + 16)
    $rawptr = [BitConverter]::ToUInt32($bytes, $o + 20)
    $e = [int64]$rawptr + [int64]$rawsz
    if ($e -gt $end) { $end = $e }
}
if ($end -gt $bytes.Length) { $end = $bytes.Length }
Write-Host ("[pack] PE 映像占用 {0:N0} 字节 (0x{1:X})，校验范围到此为止" -f $end, $end)

# ---------- 3) SHA-256（跳过 64 字节标记块）----------
# 注意: HashAlgorithm.TransformBlock 会把输入同时拷到输出缓冲, 不能传 1 字节的 sink;
#       这里直接把 [0,off) 与 [off+64,end) 两段拼成一块, 一次性 ComputeHash。
$n1 = [int]$off
$n2 = [int]($end - $off - $MarkerSize)
if ($n2 -lt 0) { $n2 = 0 }
$buf = New-Object byte[] ($n1 + $n2)
if ($n1 -gt 0) { [Array]::Copy($bytes, 0, $buf, 0, $n1) }
if ($n2 -gt 0) { [Array]::Copy($bytes, [int]($off + $MarkerSize), $buf, $n1, $n2) }
$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = $sha.ComputeHash($buf)
$sha.Dispose()
$hex = ($hash | ForEach-Object { $_.ToString('x2') }) -join ''
Write-Host "[pack] SHA-256 = $hex"

# ---------- 4) 写入哈希槽 ----------
[Array]::Copy($hash, 0, $bytes, [int]($off + $HashOff), 32)
[System.IO.File]::WriteAllBytes($Exe, $bytes)

# ---------- 5) 填充到目标大小（填充在映像之外，不参与校验）----------
$target = [int64]$TargetMB * 1MB
$cur = [int64]$bytes.Length
if ($cur -lt $target) {
    $fs = [System.IO.File]::Open($Exe, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write)
    try {
        $fs.Position = $cur
        $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
        $chunk = New-Object byte[] (1MB)
        $left = $target - $cur
        while ($left -gt 0) {
            $n = [int][Math]::Min($left, $chunk.Length)
            $rng.GetBytes($chunk)
            $fs.Write($chunk, 0, $n)
            $left -= $n
        }
    } finally { $fs.Close() }
    Write-Host ("[pack] 已填充 {0:N0} 字节随机数据" -f ($target - $cur))
} else {
    Write-Host ("[pack] 已 >= 目标大小（{0:N0} 字节），跳过填充" -f $cur)
}
$fin = (Get-Item -LiteralPath $Exe).Length
Write-Host ("[pack] 完成：{0}  最终 {1:N0} 字节 ({2:N2} MB)" -f (Split-Path $Exe -Leaf), $fin, ($fin / 1MB))
