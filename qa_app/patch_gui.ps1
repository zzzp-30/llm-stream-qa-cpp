param([string]$exePath)
$bytes = [System.IO.File]::ReadAllBytes($exePath)
$peOffset = [BitConverter]::ToInt32($bytes, 60)
$subsystemOffset = $peOffset + 92
if ([BitConverter]::ToInt16($bytes, $subsystemOffset) -ne 3) {
    $v = [BitConverter]::GetBytes([int16]3)
    $bytes[$subsystemOffset] = $v[0]
    $bytes[$subsystemOffset + 1] = $v[1]
    [System.IO.File]::WriteAllBytes($exePath, $bytes)
    Write-Host "Patched PE subsystem to Windows GUI"
}
