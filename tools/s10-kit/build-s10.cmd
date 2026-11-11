@echo off
rem ---------------------------------------------------------------------------
rem build-s10.cmd (v1.9) - build, catalog and test-sign the Galaxy S10 (beyond1lte)
rem Windows drivers: Exynos9820Hsi2c, S10Touch (touch, new in v1.5),
rem Exynos9820Ufs (-Board Beyond1), S10eBattery (battery/charging), Exynos9820WlanCx (WiFi),
rem Exynos9820Audio (speaker / earpiece: ACX, CS47L93 codec + two CS35L40 amps),
rem and S10eDiag (kernel helper + S10eAgent service: power / Bixby / volume keys).
rem v1.9: the UFS INF gets a newer DriverVer (UFSVER below) (upstream ships 9.0.0.0 for every build, so pnputil would keep an
rem older driver installed under the same number; update-ufs.ps1 swaps it in a running Windows).
rem v1.7: GPU packages (S10_GPU=1) land in out-gpu\. v1.4: GPU parked for the touch/audio/WiFi test round (UEFI v0.1.5 hides GPU0); set S10_GPU=1
rem before running this to build MaliG76Probe / MaliG76Wddm again (v1.3 behaviour, optional).
rem Same shape as the z3s build-all.cmd; run it inside an EWDK window (LaunchBuildEnv.cmd).
rem
rem Signs with the SAME test certificate as the z3s kit ("CN=z3s WoA Test", made by
rem new-cert.ps1) - nothing new to trust if it is already on this PC.
rem
rem Driver sources: next to this file (the kit zip) or ..\..\drivers (inside the repo).
rem Output: out\<Driver>\ with .sys, .inf, .cat (all signed); out\S10eDiag\ holds the key agent
rem (no .inf - make-windows.ps1 installs it from SetupComplete.cmd). make-windows.ps1 reads out\.
rem
rem The UFS driver is NOT built from its .vcxproj: the validated profile is
rem build-ufs.ps1 -WideUncached -DmaWindow (fixed cl/link lines, portable contract),
rem and only that script keeps the board identity (UfsBoardIdentity) that
rem make-windows.ps1 checks so an S10e driver can never land on the S10.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "ROOT=%~dp0"
set "OUT=%ROOT%out"
set "CERTNAME=z3s WoA Test"
set "UFSVER=11/11/2026,9.0.47.1"
set "CATOS=10_CO_ARM64,10_NI_ARM64,10_GE_ARM64"

set "SRCROOT=%ROOT%"
if not exist "%SRCROOT%Exynos9820Ufs\Exynos9820Ufs.c" set "SRCROOT=%ROOT%..\..\drivers\"
if not exist "%SRCROOT%Exynos9820Ufs\Exynos9820Ufs.c" (echo [FAIL] driver sources not found next to this file nor in ..\..\drivers & exit /b 1)
echo [ OK ] sources: %SRCROOT%

where msbuild  >nul 2>&1 || (echo [FAIL] msbuild not found - open the EWDK LaunchBuildEnv.cmd window first & exit /b 1)
where inf2cat  >nul 2>&1 || (echo [FAIL] inf2cat not found - WDK tools missing from PATH & exit /b 1)
where signtool >nul 2>&1 || (echo [FAIL] signtool not found & exit /b 1)

set "CERTSTORE="
if defined FORCE_CERTSTORE set "CERTSTORE=%FORCE_CERTSTORE%"
if not defined CERTSTORE (
  powershell -NoProfile -Command "if (Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=%CERTNAME%' -and $_.HasPrivateKey }) { exit 0 } else { exit 1 }" && set "CERTSTORE=My"
)
if not defined CERTSTORE (
  powershell -NoProfile -Command "if (Get-ChildItem Cert:\CurrentUser\PrivateCertStore -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq 'CN=%CERTNAME%' -and $_.HasPrivateKey }) { exit 0 } else { exit 1 }" && set "CERTSTORE=PrivateCertStore"
)
if not defined CERTSTORE (
  echo [FAIL] Test certificate "%CERTNAME%" not found in CurrentUser\My or PrivateCertStore.
  echo        Run once:  powershell -ExecutionPolicy Bypass -File "%ROOT%new-cert.ps1"
  exit /b 1
)
echo [ OK ] Signing with "%CERTNAME%" from CurrentUser\%CERTSTORE%

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1
rem The public half of the certificate, for install-in-windows.ps1 (pnputil on the phone must trust it).
powershell -NoProfile -Command "$c = Get-ChildItem Cert:\CurrentUser\%CERTSTORE% | Where-Object { $_.Subject -eq 'CN=%CERTNAME%' } | Select-Object -First 1; Export-Certificate -Cert $c -FilePath '%OUT%\kit-test-cert.cer' | Out-Null" || (echo [FAIL] exporting the certificate & exit /b 1)

call :msb Exynos9820Hsi2c || exit /b 1
rem v1.5: S10Touch (new touch driver, notes/s10-touch-driver.md) replaces SecTsY761Touch
call :msb S10Touch || exit /b 1
call :ufs || exit /b 1
call :msb S10eBattery || exit /b 1
rem WiFiCx client (KMDF 1.33 + NetAdapterCx 2.2); same msbuild switches as its build-package.ps1.
call :msb Exynos9820WlanCx "/p:SkipPackageVerification=true /p:ApiValidator_Enable=false" || exit /b 1
rem Audio: ACX 1.1 client, same msbuild switches as its build-package.ps1. The S10's codec, amps,
rem reset pins and rails are the S10e's (live DT: cs47l93@0 on spi@10470000, cs35l40@40/@41 on
rem hsi2c@109A0000, resets gpg1-0 / gph1-1 / gph1-2, LDO17M 1.8 V + LDO18M 1.2 V).
call :msb Exynos9820Audio "/p:SkipPackageVerification=true /p:ApiValidator_Enable=false /p:ACX_VERSION_MINOR=1" || exit /b 1
call :diag || exit /b 1
rem GPU (parked; S10_GPU=1 to build). /p:MwNoWx=1: keep /W4 but do not fail on warnings a newer SDK adds.
if not "%S10_GPU%"=="1" goto :nogpu
call :msb MaliG76Probe "/p:SkipPackageVerification=true /p:ApiValidator_Enable=false /p:MwNoWx=1" || (echo [WARN] MaliG76Probe not built - GPU test skipped & if exist "%OUT%\MaliG76Probe" rmdir /s /q "%OUT%\MaliG76Probe")
call :msb MaliG76Wddm "/p:SkipPackageVerification=true /p:ApiValidator_Enable=false /p:MwNoWx=1 /p:ExynosBoardBeyond1=1" || (echo [WARN] MaliG76Wddm not built - GPU display skipped & if exist "%OUT%\MaliG76Wddm" rmdir /s /q "%OUT%\MaliG76Wddm")
rem v1.7: the two GPU packages go to out-gpu\, NOT out\: both bind ACPI\SAMS9828 (only one may be installed), and
rem make-windows v2.3 / push-to-windows / install-in-windows take EVERYTHING in out\. gpu-s10.ps1 reads out-gpu\.
if exist "%ROOT%out-gpu" rmdir /s /q "%ROOT%out-gpu"
mkdir "%ROOT%out-gpu"
for %%G in (MaliG76Probe MaliG76Wddm) do if exist "%OUT%\%%G" move "%OUT%\%%G" "%ROOT%out-gpu\" >nul
if exist "%OUT%\kit-test-cert.cer" copy /y "%OUT%\kit-test-cert.cer" "%ROOT%out-gpu\" >nul
echo [ OK ] GPU packages in %ROOT%out-gpu ^(use gpu-s10.ps1^)
:nogpu

echo.
echo [ OK ] Drivers ready in %OUT%
dir /b /s "%OUT%"
exit /b 0

rem ---------------------------------------------------------------------------
:msb
set "NAME=%~1"
set "SRC=%SRCROOT%%NAME%"
set "DST=%OUT%\%NAME%"
echo.
echo ==== %NAME% ====
pushd "%SRC%" || exit /b 1
msbuild "%NAME%.vcxproj" /nologo /m /v:minimal /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:EnableInf2cat=false %~2
if errorlevel 1 (popd & echo [FAIL] build %NAME% & exit /b 1)
mkdir "%DST%"
set "SYS="
for /r "%SRC%\ARM64" %%F in (%NAME%.sys) do if exist "%%F" set "SYS=%%F"
if not defined SYS (popd & echo [FAIL] %NAME%.sys not found under ARM64\ & exit /b 1)
copy /y "%SYS%" "%DST%\" >nul
rem .inx projects (HSI2C) produce a stamped .inf next to the .sys; touch ships a plain .inf.
set "INF="
for /r "%SRC%\ARM64" %%F in (%NAME%.inf) do if exist "%%F" set "INF=%%F"
if not defined INF if exist "%SRC%\%NAME%.inf" set "INF=%SRC%\%NAME%.inf"
if not defined INF (popd & echo [FAIL] %NAME%.inf not found & exit /b 1)
copy /y "%INF%" "%DST%\" >nul
rem v1.8 (S10 on upstream 1.0.72): the package carries the user-mode driver the kit has (umd\maliumd.dll, the
rem Linux/mingw ARM64 build); s10-wddm-inf.ps1 drops the INF lines of the files it has not (ARM64EC, x86, OpenGL).
if /i "%NAME%"=="MaliG76Wddm" if exist "%ROOT%umd\maliumd.dll" copy /y "%ROOT%umd\maliumd.dll" "%DST%\" >nul
if /i "%NAME%"=="MaliG76Wddm" powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%s10-wddm-inf.ps1" "%DST%\%NAME%.inf"
if errorlevel 1 (popd & echo [FAIL] s10-wddm-inf.ps1 & exit /b 1)
popd
goto :catsign

rem ---------------------------------------------------------------------------
:ufs
set "NAME=Exynos9820Ufs"
set "SRC=%SRCROOT%%NAME%"
set "DST=%OUT%\%NAME%"
set "UFSBUILD=%OUT%\_ufs-build"
echo.
echo ==== %NAME% (board Beyond1 = Galaxy S10) ====
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\build-ufs.ps1" -SourceDir "%SRC%" -OutDir "%UFSBUILD%" -WideUncached -DmaWindow -Board Beyond1
if errorlevel 1 (echo [FAIL] build-ufs.ps1 & exit /b 1)
if not exist "%UFSBUILD%\%NAME%.sys" (echo [FAIL] %NAME%.sys was not produced & exit /b 1)
mkdir "%DST%"
copy /y "%UFSBUILD%\%NAME%.sys" "%DST%\" >nul
copy /y "%SRC%\%NAME%.inf" "%DST%\" >nul
powershell -NoProfile -Command "$f='%DST%\%NAME%.inf'; (Get-Content $f) -replace '^DriverVer=.*','DriverVer=%UFSVER%' | Set-Content -Encoding ASCII $f"
findstr /b /c:"DriverVer=%UFSVER%" "%DST%\%NAME%.inf" >nul || (echo [FAIL] could not stamp DriverVer & exit /b 1)
rem UfsDiag.exe stays in _ufs-build (a test tool, not part of the driver package).
goto :catsign

rem ---------------------------------------------------------------------------
:diag
rem S10eDiag (kernel MMIO helper) + S10eAgent (service) + s10ekeys/s10ereboot/...: the S10e's key
rem handling. The S10's keys are the same GPIOs (gpa0-3/4/6, gpa2-4, live DT gpio_keys), so it is
rem used unchanged. Built by its own build.bat (EWDK-aware), signed here with the kit certificate.
set "DST=%OUT%\S10eDiag"
echo.
echo ==== S10eDiag (keys agent) ====
set "S10E_SIGN_SKIP=1"
call "%SRCROOT%S10eDiag\build.bat" "%DST%"
if errorlevel 1 (echo [FAIL] S10eDiag build.bat & exit /b 1)
set "S10E_SIGN_SKIP="
del /q "%DST%\*.obj" >nul 2>&1
for %%F in ("%DST%\*.sys" "%DST%\*.exe") do call :sign "%%~F" || exit /b 1
echo [ OK ] S10eDiag
exit /b 0

rem ---------------------------------------------------------------------------
:catsign
call :sign "%DST%\%NAME%.sys" || exit /b 1
inf2cat /driver:"%DST%" /os:%CATOS% /uselocaltime
if errorlevel 1 (echo [FAIL] inf2cat %NAME% & exit /b 1)
call :sign "%DST%\%NAME%.cat" || exit /b 1
echo [ OK ] %NAME%
exit /b 0

rem ---------------------------------------------------------------------------
rem :sign <file> - up to 3 tries (antivirus often holds a file that was just
rem copied), then one verbose run so the real reason is printed.
:sign
for /l %%T in (1,1,3) do (
  signtool sign /q /fd sha256 /s %CERTSTORE% /n "%CERTNAME%" "%~1" >nul 2>&1 && exit /b 0
  timeout /t 2 /nobreak >nul
)
echo [FAIL] sign %~1 - details:
signtool sign /v /debug /fd sha256 /s %CERTSTORE% /n "%CERTNAME%" "%~1"
exit /b 1
