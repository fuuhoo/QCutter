# Fix Qt Creator MinGW Kits
# Run as: powershell -NoProfile -ExecutionPolicy Bypass -File fix_kits.ps1
# (Use the wrapper batch file fix_kits_minGW.bat, do not run directly.)

$qtDir      = $env:APPDATA + "\QtProject\qtcreator"
$profilePath = Join-Path $qtDir "profiles.xml"
$debuggerPath = Join-Path $qtDir "debuggers.xml"

if (-not (Test-Path $profilePath)) {
    Write-Host "[ERROR] profiles.xml not found at $profilePath"
    pause
    exit 1
}

# ---------- Patch debuggers.xml ----------
$qtGdb64 = 'D:/Qt/Qt5.12.12/Tools/mingw730_64/bin/gdb.exe'
$qtGdb32 = 'D:/Qt/Qt5.12.12/Tools/mingw730_32/bin/gdb.exe'

$dbgContent = Get-Content $debuggerPath -Raw
$dbgChanged = $false

if (-not $dbgContent.Contains($qtGdb64)) {
    $entry64 = @"

<data>
  <variable>DebuggerItem.99</variable>
  <valuemap type="QVariantMap">
    <valuelist type="QVariantList" key="Abis">
      <value type="QString">x86-windows-msys-pe-64bit</value>
    </valuelist>
    <value type="bool" key="AutoDetected">false</value>
    <value type="QString" key="Binary">$qtGdb64</value>
    <value type="QString" key="DetectionSource"></value>
    <value type="QString" key="DisplayName">Qt 5.12.12 MinGW 64-bit GDB</value>
    <value type="int" key="EngineType">1</value>
    <value type="QString" key="Id">{qt-minGW64-gdb-aaaa-bbbb-cccc-dddddddddddd}</value>
    <value type="QString" key="Version"></value>
    <value type="QString" key="WorkingDirectory"></value>
  </valuemap>
</data>
"@
    $dbgContent = $dbgContent.Replace('</qtcreator>', $entry64 + "</qtcreator>")
    Write-Host '  Added: Qt 5.12.12 MinGW 64-bit GDB'
    $dbgChanged = $true
}

if (-not $dbgContent.Contains($qtGdb32)) {
    $entry32 = @"

<data>
  <variable>DebuggerItem.98</variable>
  <valuemap type="QVariantMap">
    <valuelist type="QVariantList" key="Abis">
      <value type="QString">x86-windows-msys-pe-32bit</value>
    </valuelist>
    <value type="bool" key="AutoDetected">false</value>
    <value type="QString" key="Binary">$qtGdb32</value>
    <value type="QString" key="DetectionSource"></value>
    <value type="QString" key="DisplayName">Qt 5.12.12 MinGW 32-bit GDB</value>
    <value type="int" key="EngineType">1</value>
    <value type="QString" key="Id">{qt-minGW32-gdb-aaaa-bbbb-cccc-dddddddddddd}</value>
    <value type="QString" key="Version"></value>
    <value type="QString" key="WorkingDirectory"></value>
  </valuemap>
</data>
"@
    $dbgContent = $dbgContent.Replace('</qtcreator>', $entry32 + "</qtcreator>")
    Write-Host '  Added: Qt 5.12.12 MinGW 32-bit GDB'
    $dbgChanged = $true
}

if ($dbgChanged) {
    Set-Content -Path $debuggerPath -Value $dbgContent -Encoding UTF8 -NoNewline
    Write-Host '[OK] debuggers.xml updated'
} else {
    Write-Host '[OK] debuggers.xml already has Qt gdb entries'
}

# ---------- Patch profiles.xml ----------
$minGW_C   = '{8f01bbda-364f-4f0f-94c5-d3850a53b0fa}'
$minGW_Cxx = '{d0286276-efee-412f-94f2-a42c528ea332}'

$profile = Get-Content $profilePath -Raw
$profileChanged = $false

Write-Host '[1] Fixing MinGW ToolChainsV3 (QString -> QByteArray + UUID)...'
$tcPattern = '(?s)<valuemap type="QVariantMap" key="PE.Profile.ToolChainsV3">\s*<value type="QString" key="C">ProjectExplorer\.ToolChain\.Mingw:qt\.tools\.[^<]+</value>\s*<value type="QString" key="Cxx">ProjectExplorer\.ToolChain\.Mingw:qt\.tools\.[^<]+</value>\s*</valuemap>'
$tcCount = ([regex]::Matches($profile, $tcPattern)).Count
Write-Host ('  Found ' + $tcCount + ' MinGW Kit(s) with broken ToolChainsV3')
if ($tcCount -gt 0) {
    $replacement = '<valuemap type="QVariantMap" key="PE.Profile.ToolChainsV3"><value type="QByteArray" key="C">' + $minGW_C + '</value><value type="QByteArray" key="Cxx">' + $minGW_Cxx + '</value></valuemap>'
    $profile = [regex]::Replace($profile, $tcPattern, $replacement)
    $profileChanged = $true
    Write-Host '  Replaced with QByteArray UUIDs'
}

Write-Host '[2] Replacing debugger references with Qt gdb path...'
$gdbReplaced = 0
$dbgPattern = '<value type="QString" key="Debugger.Information">[^<]+</value>'
foreach ($m in [regex]::Matches($profile, $dbgPattern)) {
    $val = $m.Value
    if ($val -match 'win32_mingw730|win64_mingw730|C:/MinGW/bin/gdb') {
        $new = $val
        if ($new -match 'C:/MinGW/bin/gdb\.exe') { $new = $new.Replace('C:/MinGW/bin/gdb.exe', $qtGdb64) }
        if ($new -match 'win32_mingw730') { $new = $new.Replace('Debugger.qt.tools.win32_mingw730', $qtGdb64) }
        if ($new -match 'win64_mingw730')  { $new = $new.Replace('Debugger.qt.tools.win64_mingw730',  $qtGdb64) }
        $profile = $profile.Replace($val, $new)
        $gdbReplaced++
    }
}
Write-Host ('  Replaced ' + $gdbReplaced + ' debugger reference(s)')
if ($gdbReplaced -gt 0) { $profileChanged = $true }

if (-not $profileChanged) {
    Write-Host '[OK] profiles.xml already clean.'
    exit 0
}

Set-Content -Path $profilePath -Value $profile -Encoding UTF8 -NoNewline
Write-Host ''
Write-Host '[OK] profiles.xml patched successfully.'