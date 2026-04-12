# Flash download speed test for CMSIS-DAP + STM32G030
# Tests different SWD clock speeds using OpenOCD

$ocd = "C:\Users\SJ\.platformio\packages\tool-openocd\bin\openocd.exe"
$scriptsDir = "C:\Users\SJ\.platformio\packages\tool-openocd\openocd\scripts"
$hexFile = "C:\Users\SJ\Desktop\daplink\ph.hex"

# Get actual binary size from hex file (approximate from file size / 2.8 ratio for Intel HEX)
$hexSize = (Get-Item $hexFile).Length
Write-Host "HEX file size: $hexSize bytes"

# Test speeds in Hz
$speeds = @(1000000, 5000000, 10000000, 15000000, 20000000)

$results = @()

foreach ($speed in $speeds) {
    $speedMHz = [math]::Round($speed / 1e6, 1)
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "Testing SWD clock: ${speedMHz} MHz" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan

    # OpenOCD command: set adapter speed, connect, program, exit
    $speedKHz = [int]($speed / 1000)
    
    $ocdCmd = @"
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

    $cfgFile = "$env:TEMP\ocd_speed_test.cfg"
    $ocdCmd | Set-Content -Path $cfgFile -Encoding ASCII

    $startTime = Get-Date
    
    $proc = Start-Process -FilePath $ocd -ArgumentList "-s `"$scriptsDir`" -f `"$cfgFile`"" `
        -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\ocd_stdout.txt" -RedirectStandardError "$env:TEMP\ocd_stderr.txt"
    
    $proc | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
    
    $endTime = Get-Date
    $elapsed = ($endTime - $startTime).TotalSeconds
    
    $stderr = Get-Content "$env:TEMP\ocd_stderr.txt" -Raw -ErrorAction SilentlyContinue
    
    # Check for success
    $success = $false
    $actualSpeed = ""
    if ($stderr -match "verified") {
        $success = $true
    }
    if ($stderr -match "adapter speed:\s*(\d+)\s*kHz") {
        $actualSpeed = "$($matches[1]) kHz"
    }
    
    if (-not $proc.HasExited) {
        $proc | Stop-Process -Force
        Write-Host "TIMEOUT - killed after 30s" -ForegroundColor Red
    }

    $status = if ($success) { "OK" } else { "FAIL" }
    $color = if ($success) { "Green" } else { "Red" }
    
    Write-Host "Status: $status  Time: $([math]::Round($elapsed, 2))s  Actual speed: $actualSpeed" -ForegroundColor $color
    
    if (-not $success) {
        # Show last few error lines
        $lines = ($stderr -split "`n") | Select-Object -Last 5
        foreach ($l in $lines) { Write-Host "  $l" -ForegroundColor Yellow }
    }
    
    $results += [PSCustomObject]@{
        SpeedMHz = $speedMHz
        SpeedKHz = $speedKHz
        ActualSpeed = $actualSpeed
        Status = $status
        TimeSeconds = [math]::Round($elapsed, 2)
    }
    
    # Brief pause between tests
    Start-Sleep -Seconds 1
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "RESULTS SUMMARY" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
$results | Format-Table -AutoSize
