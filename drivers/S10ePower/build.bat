@echo off
rem S10ePower build (ARM64): S10ePower.sys + s10epowerctl.exe + s10egovernor.exe + cpusets.exe + cpustress.exe
rem + clockscli.exe + clocks.exe -> %1 (default %TEMP%\s10ediag)
rem Same toolchain/flags as the Exynos9820Ufs exact build; test-signed with the WDK test certificate.
rem exynos-woa S10 kit: works inside an EWDK window too (kit root / version / vcvars from the EWDK, kit ucrt appended),
rem and S10E_SIGN_SKIP=1 leaves signing to the caller (build-s10.cmd signs with the kit certificate) - as S10eDiag.
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\s10ediag
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
cl.exe /c /nologo /W4 /WX /Ox /Os /D _ARM64_ /D ARM64 /D _USE_DECLSPECS_FOR_SAL=1 /D STD_CALL /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000010 /GF /Gm- /Zp8 /GS /guard:cf /Gy /Zc:wchar_t- /GR- /kernel /FI"%KIT%\Include\%KV%\shared\warning.h" /Fo"%OUT%\S10ePower.obj" "%SRC%S10ePower.c"
if errorlevel 1 exit /b 2
link.exe /NOLOGO /WX /NODEFAULTLIB /MANIFEST:NO /DEBUG:NONE /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" /RELEASE /Brepro /MACHINE:ARM64 /guard:cf /kernel /osversion:10.0 /OUT:"%OUT%\S10ePower.sys" "%OUT%\S10ePower.obj" "%KIT%\Lib\%KV%\km\arm64\BufferOverflowFastFailK.lib" "%KIT%\Lib\%KV%\km\arm64\ntoskrnl.lib" "%KIT%\Lib\%KV%\km\arm64\hal.lib" "%KIT%\Lib\%KV%\km\arm64\wdmsec.lib" "%KIT%\Lib\%KV%\km\arm64\aux_klib.lib" "%KIT%\Lib\%KV%\um\arm64\arm64rt.lib"
if errorlevel 1 exit /b 3
del "%OUT%\S10ePower.obj" >nul 2>&1
echo S10EPOWER_BUILD_OK %OUT%

set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%SAVED_INCLUDE%
set LIB=%KIT%\Lib\%KV%\um\arm64;%KIT%\Lib\%KV%\ucrt\arm64;%SAVED_LIB%
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\s10epowerctl.obj" /Fe"%OUT%\s10epowerctl.exe" "%SRC%s10epowerctl.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 5
del "%OUT%\s10epowerctl.obj" >nul 2>&1
echo S10EPOWERCTL_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\s10egovernor.obj" /Fe"%OUT%\s10egovernor.exe" "%SRC%s10egovernor.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro advapi32.lib user32.lib pdh.lib
if errorlevel 1 exit /b 6
del "%OUT%\s10egovernor.obj" >nul 2>&1
echo S10EGOVERNOR_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /Fo"%OUT%\cpusets.obj" /Fe"%OUT%\cpusets.exe" "%SRC%cpusets.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 7
del "%OUT%\cpusets.obj" >nul 2>&1
echo CPUSETS_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /Fo"%OUT%\cpustress.obj" /Fe"%OUT%\cpustress.exe" "%SRC%cpustress.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 9
del "%OUT%\cpustress.obj" >nul 2>&1
echo CPUSTRESS_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /I "%SRC%..\MaliG76Wddm\include" /Fo"%OUT%\clockscli.obj" /Fe"%OUT%\clockscli.exe" "%SRC%clockscli.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE gdi32.lib advapi32.lib /Brepro
if errorlevel 1 exit /b 10
del "%OUT%\clockscli.obj" >nul 2>&1
echo CLOCKSCLI_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /I "%SRC%..\MaliG76Wddm\include" /Fo"%OUT%\clocks.obj" /Fe"%OUT%\clocks.exe" "%SRC%clocks.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS gdi32.lib user32.lib advapi32.lib /Brepro
if errorlevel 1 exit /b 11
del "%OUT%\clocks.obj" >nul 2>&1
echo CLOCKS_BUILD_OK

rem S10E_SIGN_SKIP=1: the caller signs (the Galaxy S10 kit signs with its own test certificate).
if defined S10E_SIGN_SKIP goto :signed
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\S10ePower.sys" "%OUT%\s10epowerctl.exe" "%OUT%\s10egovernor.exe" "%OUT%\clockscli.exe" "%OUT%\clocks.exe" >nul
if errorlevel 1 exit /b 4
:signed
echo S10EPOWER_ALL_OK %OUT%
