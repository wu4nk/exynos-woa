@echo off
rem ---------------------------------------------------------------------------
rem build-s10.cmd (v1.0) - build, catalog and test-sign the Galaxy S10 (beyond1lte)
rem Windows drivers: Exynos9820Hsi2c, S6SY761TouchBeyond0, Exynos9820Ufs (-Board Beyond1).
rem Same shape as the z3s build-all.cmd; run it inside an EWDK window (LaunchBuildEnv.cmd).
rem
rem Signs with the SAME test certificate as the z3s kit ("CN=z3s WoA Test", made by
rem new-cert.ps1) - nothing new to trust if it is already on this PC.
rem
rem Driver sources: next to this file (the kit zip) or ..\..\drivers (inside the repo).
rem Output: out\<Driver>\ with .sys, .inf, .cat (all signed). make-windows.ps1 reads out\.
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

call :msb Exynos9820Hsi2c || exit /b 1
call :msb S6SY761TouchBeyond0 || exit /b 1
call :ufs || exit /b 1

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
msbuild "%NAME%.vcxproj" /nologo /m /v:minimal /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:EnableInf2cat=false
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
rem UfsDiag.exe stays in _ufs-build (a test tool, not part of the driver package).
goto :catsign

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
