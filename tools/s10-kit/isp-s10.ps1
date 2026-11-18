<#
  isp-s10.ps1 (v1.10) - Galaxy S10: the camera ISP's UMDF 2 driver S10eIsp (notes/s10-isp.md, Stage 4 option A).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder (out\S10eIsp\ from build-s10.cmd v1.19+).
  Needs beyond1lte-uefi v0.2.3+ (its DSDT has the ISP node CISP = ACPI\SAMS982C, present while pd-isppre is ON).
      .\isp-s10.ps1 install   install the driver package (pnputil); Windows binds it to ACPI\SAMS982C and the probe
                              runs once when the device starts. Then: .\isp-s10.ps1 status
      .\isp-s10.ps1 status    the device, its driver and problem code, and S10eIsp's probe report (registry
                              Enum\ACPI\SAMS982C\<instance>\Device Parameters\S10eIsp, value Probe)
      .\isp-s10.ps1 restart   disable + enable the device: the probe runs again
      .\isp-s10.ps1 remove    remove the driver package (the device stays, without a driver)
      .\isp-s10.ps1 dma       (v1.1) install + start S10eIspDma (out\S10eIspDma\S10eIspDma.sys -> C:\S10Prov\ispdma,
                              kernel service, SYSTEM start: its contiguous pool below 4 GiB is taken at boot, when
                              there still is one). Then .\isp-s10.ps1 restart: S10eIsp 0.2 maps the pool and tests it.
      .\isp-s10.ps1 dmaremove stop + delete the S10eIspDma service
      .\isp-s10.ps1 front     (v1.3, S10eIsp 0.3 + S10eIspDma) Samsung's ISP libraries run INSIDE the driver's Wudfhost:
                              the front path, 30 frames, FAKE register windows (no ISP register is touched), the REAL
                              DMA pool. Needs C:\S10Prov\isp\fimc_is_lib.bin, fimc_is_rta.bin, setfile_3j1.bin
                              (rom1_cal.bin optional). Gives LocalService (Wudfhost's account) write access to
                              C:\S10Prov\isp, drops run.flg, restarts the device, waits up to 3 min, zips
                              C:\S10Prov\isp\out to C:\S10Prov\isp-front.zip - send that zip.
                              v1.4 (S10eIsp 0.3.1): -NoDma = the libraries get a fake pool, S10eIspDma is not used
                              (bisect switch after the blue screen of 2026-11-18). C:\S10Prov\isp\front-trace.txt
                              (written through to disk) shows how far the run thread got, even after a crash.
                              v1.5 (S10eIsp 0.4): -Hw3aa = the libraries' INIT steps (3AA chain_create /
                              set_system_config / object_create) on the REAL 3AA0 - the first ISP register writes
                              from Windows; the run stops after them (no frame, no DMA address, no interrupt). All
                              3AA0 registers read before / after; the changed ones are listed. Camera apps closed.
                              v1.6 (S10eIsp 0.5): -Hw3aaShot = -Hw3aa + set_param / set_ctrl / shot F1 on the real
                              3AA0 (DMA addresses = the pool's PAs, no frame comes in), 200 ms watching its interrupt
                              status, then the DDK's stop.
                              v1.7 (S10eIsp 0.6 + UEFI v0.2.4): 3AA0's interrupts are connected; the registry value
                              Irq (counts, which GSIV each interrupt object got) is printed by front and status.
                              v1.8 (S10eIsp 0.7 + S10eCam 0.14): -Hw3aaOtf = -Hw3aaShot told the sensor runs
                              mode 11 (912x684); when the 3AA0 is armed this script runs .\cam-s10.ps1 shot -Bin -Otf
                              -Frames 30 (CSIS2 -> PDP0 -> 3AA0), the run saves the 3AA's output buffers (out\otf-*.raw,
                              in the zip). Camera apps closed (the stream must be free).
                              v1.9 (S10eIsp 0.8 + UEFI v0.2.5): -Hw3aaOtfIsp = -Hw3aaOtf + ISP0 on the real ISPLP,
                              M2M per frame to a YUY2 buffer (out\otf-isp-yuy2-912x684.raw).
      .\isp-s10.ps1 pipe      (v1.10, S10eIsp 0.9 + S10eIspDma 0.2 + S10eCamera 0.10) switch the CAMERA PIPELINE on:
                              C:\S10Prov\isp\pipe.flg (kept: every device start / boot), restart the device, show
                              its trace. The Windows camera then gets Samsung's ISP frames. -Off switches it off.
  The v0.1 probe READS: CMU_ISPPRE 3AA0 gate + LHM_AXI_P, and (if both on) the 3AA0 ID registers - the reads
  S10eCam 0.11 already made from the kernel on this phone. It never writes a register, never reads ISP0 / MCSC.
  Only "front -Hw3aa / -Hw3aaShot" write registers (3AA0, by Samsung's libraries' init steps).
  Writes C:\S10Prov\isp-s10-<action>.txt - send it back.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'status', 'restart', 'remove', 'dma', 'dmaremove', 'front', 'pipe')][string]$Action = 'status',
      [switch]$NoDma, [switch]$Hw3aa, [switch]$Hw3aaShot, [switch]$Hw3aaOtf, [switch]$Hw3aaOtfIsp, [switch]$Off, [switch]$Bin)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\isp-s10-$Action.txt"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Remove-Item $Out -Force -ErrorAction SilentlyContinue
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
W "isp-s10.ps1 v1.11  $Action  $(Get-Date -Format s)"
$fw = (Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue).BIOSVersion
W "firmware: $fw"

function Dev { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ACPI\SAMS982C\*' } }

function Status {
    $d = Dev
    if (-not $d) {
        W 'device ACPI\SAMS982C: NOT PRESENT - UEFI older than v0.2.3, or pd-isppre was off when ACPI evaluated _STA'
        return
    }
    foreach ($x in $d) {
        W "device: $($x.InstanceId)  status $($x.Status)  problem $($x.Problem)  '$($x.FriendlyName)'"
        $drv = (Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        $ver = (Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
        W "driver: $drv $ver"
        $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($x.InstanceId)\Device Parameters\S10eIsp"
        $p = Get-ItemProperty $k -ErrorAction SilentlyContinue
        if ($p -and $p.Probe) {
            W "probe report (S10eIsp $($p.Version)):"
            foreach ($l in ($p.Probe -split "`r`n")) { if ($l) { W "  $l" } }
            if ($p.Irq) { W "interrupts (value Irq): $($p.Irq)" }
        } else {
            W "no probe report under $k (driver not loaded yet, or it failed before writing)"
        }
    }
    $q = (sc.exe query S10eIspDma 2>&1 | Out-String)
    if ($q -match 'STATE\s*:\s*\d+\s+(\w+)') { W "S10eIspDma service: $($Matches[1])" } else { W 'S10eIspDma service: not installed' }
    $wudf = Get-Process WUDFHost -ErrorAction SilentlyContinue
    W "WUDFHost processes: $(@($wudf).Count)"
}

switch ($Action) {
    'install' {
        $inf = Join-Path $PSScriptRoot 'out\S10eIsp\S10eIsp.inf'
        if (-not (Test-Path $inf)) { $inf = Join-Path $PSScriptRoot 'S10eIsp\S10eIsp.inf' }
        if (-not (Test-Path $inf)) { W "S10eIsp.inf not found (out\S10eIsp\ from build-s10.cmd v1.19)"; break }
        W "pnputil /add-driver $inf /install"
        (pnputil /add-driver $inf /install 2>&1) | ForEach-Object { W "  $_" }
        Start-Sleep -Seconds 3
        Status
    }
    'status' { Status }
    'dma' {
        $sys = Join-Path $PSScriptRoot 'out\S10eIspDma\S10eIspDma.sys'
        if (-not (Test-Path $sys)) { $sys = Join-Path $PSScriptRoot 'S10eIspDma\S10eIspDma.sys' }
        if (-not (Test-Path $sys)) { W 'S10eIspDma.sys not found (out\S10eIspDma\ from build-s10.cmd v1.20)'; break }
        $dir = 'C:\S10Prov\ispdma'
        New-Item -ItemType Directory -Force $dir | Out-Null
        sc.exe stop S10eIspDma 2>&1 | Out-Null
        Copy-Item -Force $sys $dir
        sc.exe query S10eIspDma 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { sc.exe create S10eIspDma type= kernel start= system binPath= "$dir\S10eIspDma.sys" 2>&1 | ForEach-Object { W "  $_" } }
        else { sc.exe config S10eIspDma binPath= "$dir\S10eIspDma.sys" start= system 2>&1 | ForEach-Object { W "  $_" } }
        sc.exe start S10eIspDma 2>&1 | ForEach-Object { W "  $_" }
        Status
    }
    'front' {
        $isp = 'C:\S10Prov\isp'
        $miss = @('fimc_is_lib.bin', 'fimc_is_rta.bin', 'setfile_3j1.bin') | Where-Object { -not (Test-Path (Join-Path $isp $_)) }
        if ($miss) { W "missing in ${isp}: $($miss -join ', ') (from s10cam.zip\files)"; break }
        $d = Dev
        if (-not $d) { Status; break }
        W "icacls $isp /grant *S-1-5-19:(OI)(CI)M   (LocalService = Wudfhost)"
        (icacls $isp /grant '*S-1-5-19:(OI)(CI)M' 2>&1) | ForEach-Object { W "  $_" }
        if (Test-Path "$isp\out") { Remove-Item -Recurse -Force "$isp\out" }
        Remove-Item "$isp\front-trace.txt" -ErrorAction SilentlyContinue
        $flg = 'run'
        if ($NoDma) { $flg += ' nodma' }
        if ($Hw3aaOtfIsp) { $flg += ' hw3aaotfisp'; $Hw3aaOtf = [switch]$true } elseif ($Hw3aaOtf) { $flg += ' hw3aaotf' } elseif ($Hw3aaShot) { $flg += ' hw3aashot' } elseif ($Hw3aa) { $flg += ' hw3aa' }
        Set-Content -Path "$isp\run.flg" -Value $flg -Encoding ascii
        W "run.flg: $(Get-Content "$isp\run.flg")"
        $id = @($d)[0].InstanceId
        $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters\S10eIsp"
        Remove-ItemProperty -Path $k -Name FrontRun -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $k -Name Irq -ErrorAction SilentlyContinue
        W "pnputil /restart-device $id"
        (pnputil /restart-device "$id" 2>&1) | ForEach-Object { W "  $_" }
        $t0 = Get-Date; $fr = ''
        if ($Hw3aaOtf) {
            $armed = $false
            while (((Get-Date) - $t0).TotalSeconds -lt 60) {
                Start-Sleep -Milliseconds 500
                if ((Test-Path "$isp\front-trace.txt") -and (Select-String -Path "$isp\front-trace.txt" -Pattern 'waiting for OTF frames' -Quiet)) { $armed = $true; break }
                $fr = (Get-ItemProperty $k -ErrorAction SilentlyContinue).FrontRun
                if ($fr -match '^(finished|not run)') { break }
            }
            W "3AA0 armed: $armed  ($([int]((Get-Date) - $t0).TotalSeconds) s)"
            if ($armed) {
                W 'cam-s10.ps1 shot -Bin -Otf -Frames 30'
                & "$PSScriptRoot\cam-s10.ps1" shot -Bin -Otf -Frames 30 *>&1 | Out-Null
                if (Test-Path 'C:\S10Prov\cam-s10-shot.txt') {
                    Get-Content 'C:\S10Prov\cam-s10-shot.txt' | Where-Object { $_ -match 'OTF|frames|FRM_CNT|error|failed|INT_SRC' } | Select-Object -First 25 | ForEach-Object { W "  cam: $_" }
                    Copy-Item 'C:\S10Prov\cam-s10-shot.txt' "$isp\out\cam-s10-shot.txt" -ErrorAction SilentlyContinue
                }
            }
        }
        while (((Get-Date) - $t0).TotalSeconds -lt 180) {
            Start-Sleep -Seconds 3
            $fr = (Get-ItemProperty $k -ErrorAction SilentlyContinue).FrontRun
            if ($fr -match '^(finished|not run)') { break }
        }
        W "FrontRun: $fr  ($([int]((Get-Date) - $t0).TotalSeconds) s)"
        if (Test-Path "$isp\front-trace.txt") { W 'front-trace.txt:'; Get-Content "$isp\front-trace.txt" | ForEach-Object { W "  $_" } }
        $log = "$isp\out\host.log"
        if (Test-Path $log) {
            $l = Get-Content $log
            W "host.log: $(@($l).Count) lines, steps $(@($l | Where-Object { $_ -match '^== ' }).Count), not 0: $(@($l | Where-Object { $_ -match '^== ' -and $_ -notmatch '-> 0x0$' }).Count)"
            $l | Where-Object { $_ -match 'isphost: image|start_up|CRASH|ASSERT|time-out|steady|done:|^hw3aa' } | Select-Object -Last 14 | ForEach-Object { W "  $_" }
            $hw = @($l | Where-Object { $_ -match '^hw 3AA0 ' })
            if ($hw.Count) { W "REAL 3AA0 register changes: $($hw.Count) (all in host.log)"; $hw | Select-Object -First 40 | ForEach-Object { W "  $_" } }
            $l | Where-Object { $_ -match '^hw3aa: (after shot|\+|real interrupts)|^hw 3AA0 stop|^hw irq|^hw3aa otf|^hwisp|^== ISP' } | Select-Object -First 80 | ForEach-Object { W "  $_" }
            $l | Where-Object { $_ -match '^hw3aa otf: (done|before|after)|^hwisp: ISP0 YUY2|getIspFrameDesc' } | Select-Object -First 10 | ForEach-Object { W "  result: $_" }
            Compress-Archive -Force -Path "$isp\out\*" -DestinationPath 'C:\S10Prov\isp-front.zip'
            W 'zip: C:\S10Prov\isp-front.zip'
        } else { W "no $log" }
        Status
    }
    'pipe' {
        $isp = 'C:\S10Prov\isp'
        if ($Off) {
            Remove-Item "$isp\pipe.flg" -ErrorAction SilentlyContinue
            W 'pipe.flg removed: the camera pipeline is off (CPU conversion in S10eCamera)'
        } else {
            $miss = @('fimc_is_lib.bin', 'fimc_is_rta.bin', 'setfile_3j1.bin') | Where-Object { -not (Test-Path "$isp\$_") }
            if ($miss) { W "missing in ${isp}: $($miss -join ', ')"; break }
            (icacls $isp /grant '*S-1-5-19:(OI)(CI)M' 2>&1) | Out-Null
            Set-Content -Path "$isp\pipe.flg" -Value $(if ($Bin) { 'on bin' } else { 'on' }) -Encoding ascii
            W "pipe.flg written ($(Get-Content "$isp\pipe.flg")): the camera pipeline starts with the device (and at every boot); full sensor unless -Bin"
        }
        Remove-Item "$isp\front-trace.txt" -ErrorAction SilentlyContinue
        if (Test-Path "$isp\out") { Remove-Item -Recurse -Force "$isp\out" -ErrorAction SilentlyContinue }
        $d = Dev
        if (-not $d) { Status; break }
        W "pnputil /restart-device $(@($d)[0].InstanceId)"
        (pnputil /restart-device "$(@($d)[0].InstanceId)" 2>&1) | ForEach-Object { W "  $_" }
        Start-Sleep -Seconds 8
        if (Test-Path "$isp\front-trace.txt") { W 'front-trace.txt:'; Get-Content "$isp\front-trace.txt" | ForEach-Object { W "  $_" } }
        if (Test-Path "$isp\out\host.log") { Get-Content "$isp\out\host.log" | Where-Object { $_ -match '^pipe:|ERROR\]\[|start_up' } | Select-Object -Last 15 | ForEach-Object { W "  $_" } }
        Status
    }
    'dmaremove' {
        sc.exe stop S10eIspDma 2>&1 | ForEach-Object { W "  $_" }
        sc.exe delete S10eIspDma 2>&1 | ForEach-Object { W "  $_" }
        Status
    }
    'restart' {
        $d = Dev
        if (-not $d) { Status; break }
        foreach ($x in $d) {
            # v1.2: pnputil /restart-device (v1.1's silenced Disable/Enable-PnpDevice left the old report in place)
            W "pnputil /restart-device $($x.InstanceId)"
            (pnputil /restart-device "$($x.InstanceId)" 2>&1) | ForEach-Object { W "  $_" }
            if ($LASTEXITCODE -ne 0) {
                W "  pnputil exit $LASTEXITCODE - Disable / Enable-PnpDevice instead"
                try { Disable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false -ErrorAction Stop; Start-Sleep -Seconds 2
                      Enable-PnpDevice -InstanceId $x.InstanceId -Confirm:$false -ErrorAction Stop } catch { W "  $_" }
            }
        }
        Start-Sleep -Seconds 3
        Status
    }
    'remove' {
        $pk = (pnputil /enum-drivers 2>$null | Out-String) -split "(\r?\n){2,}" | Where-Object { $_ -match 's10eisp\.inf' }
        foreach ($b in $pk) {
            if ($b -match 'Published Name\s*:\s*(oem\d+\.inf)') {
                W "pnputil /delete-driver $($Matches[1]) /uninstall /force"
                (pnputil /delete-driver $Matches[1] /uninstall /force 2>&1) | ForEach-Object { W "  $_" }
            }
        }
        Status
    }
}
W "log: $Out"
