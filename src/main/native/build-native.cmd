chcp 65001 >nul
@echo off
setlocal

set "ROOT=%~dp0..\..\.."
set "OUT=%ROOT%\target\native"
set "SRC=%ROOT%\src\main\native\webviewer_capture.cpp"
set "MSVC=D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
set "SDK=D:\Windows Kits\10"

if not defined JAVA_HOME (
  echo JAVA_HOME is required to build the native capture DLL.
  exit /b 1
)

for /f "delims=" %%v in ('dir /b /ad "%SDK%\Include" ^| sort /r') do (
  set "SDKVER=%%v"
  goto :found_sdk
)

:found_sdk
if not defined SDKVER (
  echo Windows SDK include directory was not found under %SDK%\Include.
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

"%MSVC%\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /utf-8 /EHsc /O2 /LD ^
  /Fo"%OUT%\\" ^
  /I"%JAVA_HOME%\include" ^
  /I"%JAVA_HOME%\include\win32" ^
  /I"%ROOT%\src\main\native" ^
  /I"%MSVC%\include" ^
  /I"%SDK%\Include\%SDKVER%\cppwinrt" ^
  /I"%SDK%\Include\%SDKVER%\winrt" ^
  /I"%SDK%\Include\%SDKVER%\um" ^
  /I"%SDK%\Include\%SDKVER%\shared" ^
  /I"%SDK%\Include\%SDKVER%\ucrt" ^
  "%SRC%" ^
  /link /NOLOGO /OUT:"%OUT%\webviewer_capture.dll" ^
  /IMPLIB:"%OUT%\webviewer_capture.lib" ^
  /LIBPATH:"%MSVC%\lib\x64" ^
  /LIBPATH:"%SDK%\Lib\%SDKVER%\um\x64" ^
  /LIBPATH:"%SDK%\Lib\%SDKVER%\ucrt\x64" ^
  d3d11.lib dxgi.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib user32.lib windowsapp.lib runtimeobject.lib ole32.lib avrt.lib

exit /b %ERRORLEVEL%
