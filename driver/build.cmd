@echo off
setlocal
if not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Run in an x64 MSVC developer command prompt.
    exit /b 1
)
if "%~1"=="" (
    echo Usage: driver\build.cmd WDK_KIT_ROOT [KIT_VERSION]
    exit /b 1
)
set "stdio_kit=%~f1"
set "stdio_ver=%~2"
if not defined stdio_ver set "stdio_ver=10.0.26100.0"
set "stdio_inc=%stdio_kit%\Include\%stdio_ver%"
set "stdio_lib=%stdio_kit%\Lib\%stdio_ver%\km\x64"
if not exist "%stdio_inc%\km\fltkernel.h" (
    echo WDK kernel headers missing under "%stdio_inc%".
    exit /b 1
)
set "stdio_src=%~dp0"
set "stdio_out=%~dp0..\build\driver"
if not exist "%stdio_out%" mkdir "%stdio_out%"
pushd "%stdio_out%" || exit /b 1
cl /nologo /c /TC /std:c17 /kernel /GS /guard:cf /Zl /W4 /WX /O2 /Z7 /D_AMD64_ /D_WIN64 /DNTDDI_VERSION=0x0A000010 /D_WIN32_WINNT=0x0A00 /I"%stdio_inc%\km" /I"%stdio_inc%\shared" "%stdio_src%filter.c"
if errorlevel 1 goto failed
link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /NODEFAULTLIB /MACHINE:X64 /GUARD:CF /DYNAMICBASE /NXCOMPAT /INTEGRITYCHECK /DEBUG:FULL /OUT:stdio.sys filter.obj "%stdio_lib%\ntoskrnl.lib" "%stdio_lib%\fltmgr.lib" "%stdio_lib%\BufferOverflowFastFailK.lib"
if errorlevel 1 goto failed
copy /Y "%stdio_src%stdio.inf" stdio.inf >nul
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
