@echo off
rem S10eIspDma build (ARM64): S10eIspDma.sys (DMA pool for the ISP host, notes/s10-isp.md) -> %1 (default %TEMP%\s10eispdma)
rem Same toolchain/flags as the Exynos9820Ufs exact build; test-signed with the WDK test certificate.
rem exynos-woa S10 kit: works inside an EWDK window too (kit root / version / vcvars from the EWDK, kit ucrt appended),
rem and S10E_SIGN_SKIP=1 leaves signing to the caller (build-s10.cmd signs with the kit certificate) - as S10eDiag.
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\s10eispdma
if not exist "%OUT%" mkdir "%OUT%"
set "KIT=C:\Program Files (x86)\Windows Kits\10"
set "KV=10.0.26100.0"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if defined WDKContentRoot set "KIT=%WDKContentRoot%"
if "%KIT:~-1%"=="\" set "KIT=%KIT:~0,-1%"
if defined Version_Number set "KV=%Version_Number%"
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%VSINSTALLDIR%VC\Auxiliary\Build\vcvarsall.bat"
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%VSINSTALLDIR%\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARS%" x64_arm64 >nul
if errorlevel 1 exit /b 1
set SAVED_INCLUDE=%INCLUDE%
set SAVED_LIB=%LIB%

set INCLUDE=%KIT%\Include\%KV%\km;%KIT%\Include\%KV%\shared;%SAVED_INCLUDE%;%KIT%\Include\%KV%\ucrt
cl.exe /c /nologo /W4 /WX /Ox /Os /D _ARM64_ /D ARM64 /D _USE_DECLSPECS_FOR_SAL=1 /D STD_CALL /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000010 /GF /Gm- /Zp8 /GS /guard:cf /Gy /Zc:wchar_t- /GR- /kernel /FI"%KIT%\Include\%KV%\shared\warning.h" /Fo"%OUT%\S10eIspDma.obj" "%SRC%S10eIspDma.c"
if errorlevel 1 exit /b 2
link.exe /NOLOGO /WX /NODEFAULTLIB /MANIFEST:NO /DEBUG:NONE /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" /RELEASE /Brepro /MACHINE:ARM64 /guard:cf /kernel /osversion:10.0 /OUT:"%OUT%\S10eIspDma.sys" "%OUT%\S10eIspDma.obj" "%KIT%\Lib\%KV%\km\arm64\BufferOverflowFastFailK.lib" "%KIT%\Lib\%KV%\km\arm64\ntoskrnl.lib" "%KIT%\Lib\%KV%\km\arm64\hal.lib" "%KIT%\Lib\%KV%\km\arm64\wdmsec.lib" "%KIT%\Lib\%KV%\km\arm64\ntstrsafe.lib" "%KIT%\Lib\%KV%\um\arm64\arm64rt.lib"
if errorlevel 1 exit /b 3
del "%OUT%\S10eIspDma.obj" >nul 2>&1
copy /y "%SRC%S10eIspDmaIoctl.h" "%OUT%\" >nul

rem S10E_SIGN_SKIP=1: the caller signs (the Galaxy S10 kit signs with its own test certificate).
if defined S10E_SIGN_SKIP goto :signed
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\S10eIspDma.sys" >nul
if errorlevel 1 exit /b 4
:signed
echo S10EISPDMA_BUILD_OK %OUT%
