# Flash download speed test - cleaner version
$ocd = "C:\Users\SJ\.platformio\packages\tool-openocd\bin\openocd.exe"
$scriptsDir = "C:\Users\SJ\.platformio\packages\tool-openocd\openocd\scripts"
$hexFile = "C:\Users\SJ\Desktop\daplink\ph.hex"
$cfgFile = "$env:TEMP\ocd_speed_test.cfg"

$hexSize = (Get-Item $hexFile).Length
Write-Host "HEX file: $hexSize bytes"

$speeds = @(1000, 2000, 5000, 10000, 15000, 20000)
$results = @()

foreach ($speedKHz in $speeds) {
    $cfg = @"
adapter driver cmsis-dap
adapter speed $speedKHz
transport select swd
source [find target/stm32g0x.cfg]
init
targets
reset halt
program {$hexFile} verify
reset run
shutdown
"@
    [System.IO.File]::WriteAllText($cfgFile, $cfg, [System.Text.Encoding]::ASCII)

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $ocd -ArgumentList "-s `"$scriptsDir`" -f `"$cfgFile`"" `
        -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\ocd_out.txt" -RedirectStandardError "$env:TEMP\ocd_err.txt"
    $proc | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
    $sw.Stop()

    if (-not $proc.HasExited) { $proc | Stop-Process -Force }

    $stderr = [System.IO.File]::ReadAllText("$env:TEMP\ocd_err.txt")
    $ok = $stderr -match "Verified OK"
    $actualKHz = ""
    if ($stderr -match "clock speed (\d+) kHz") { $actualKHz = $matches[1] }
    $wroteKB = ""
    if ($stderr -match "wrote (\d+) bytes") { $wroteKB = [math]::Round([int]$matches[1]/1024, 1) }
    $elapsed = [math]::Round($sw.Elapsed.TotalSeconds, 2)

    $kbps = ""
    if ($ok -and $wroteKB) { $kbps = [math]::Round([double]$wroteKB / $elapsed, 2) }

    $results += [PSCustomObject]@{
        "Request(kHz)" = $speedKHz
        "Actual(kHz)"  = $actualKHz
        "Status"       = if($ok){"OK"}else{"FAIL"}
        "Time(s)"      = $elapsed
        "Size(KB)"     = $wroteKB
        "Speed(KB/s)"  = $kbps
    }
    Write-Host "$speedKHz kHz -> $(if($ok){'OK'}else{'FAIL'}) ${elapsed}s actual=${actualKHz}kHz"
    Start-Sleep -Seconds 1
}

Write-Host "`nRESULTS:"
$results | Format-Table -AutoSize
