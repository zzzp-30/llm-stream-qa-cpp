param([string]$exePath)
$bytes = [System.IO.File]::ReadAllBytes($exePath)
$peOffset = [BitConverter]::ToInt32($bytes, 60)
$subsystemOffset = $peOffset + 92
# PE Subsystem: 2 = Windows GUI (无控制台), 3 = Windows Console
if ([BitConverter]::ToInt16($bytes, $subsystemOffset) -ne 2) {
    $v = [BitConverter]::GetBytes([int16]2)
    $bytes[$subsystemOffset] = $v[0]
    $bytes[$subsystemOffset + 1] = $v[1]
    [System.IO.File]::WriteAllBytes($exePath, $bytes)
    Write-Host "Patched PE subsystem to Windows GUI (2)"
} else {
    Write-Host "Already Windows GUI subsystem (2)"
}
