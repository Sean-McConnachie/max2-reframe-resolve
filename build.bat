@echo off
rem Builds the OFX plugin bundle and the max2render test tool into build\
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

set OUT=build
set OBJ=build\obj
if not exist %OBJ%\sdk mkdir %OBJ%\sdk
if not exist %OBJ%\core mkdir %OBJ%\core

set CFLAGS=/nologo /O2 /EHsc /MD /std:c++17 /W3 /DWIN32 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Ithird_party\openfx\include /Ithird_party\openfx\Support\include /Isrc
set LIBS=mfplat.lib mfreadwrite.lib mfuuid.lib d3d11.lib dxgi.lib ole32.lib propsys.lib user32.lib

rem OpenFX support library (only rebuilt when missing)
set SDK=third_party\openfx\Support\Library
if not exist %OBJ%\sdk\ofxsImageEffect.obj (
  cl %CFLAGS% /c %SDK%\ofxsCore.cpp %SDK%\ofxsImageEffect.cpp %SDK%\ofxsInteract.cpp %SDK%\ofxsLog.cpp %SDK%\ofxsMultiThread.cpp %SDK%\ofxsParams.cpp %SDK%\ofxsProperty.cpp %SDK%\ofxsPropertyValidation.cpp /Fo%OBJ%\sdk\ || exit /b 1
)

cl %CFLAGS% /c src\*.cpp /Fo%OBJ%\core\ || exit /b 1

set CORE=%OBJ%\core\mp4.obj %OBJ%\core\source360.obj %OBJ%\core\decoder.obj %OBJ%\core\reproject.obj %OBJ%\core\engine.obj %OBJ%\core\log.obj

cl %CFLAGS% tools\max2render.cpp %CORE% /Fo%OBJ%\ /Fe%OUT%\max2render.exe /link %LIBS% || exit /b 1

set BUNDLE=%OUT%\Max2Reframe.ofx.bundle\Contents\Win64
if not exist %BUNDLE% mkdir %BUNDLE%
link /nologo /DLL /OUT:%BUNDLE%\Max2Reframe.ofx %OBJ%\core\plugin.obj %CORE% %OBJ%\sdk\*.obj %LIBS% || exit /b 1
echo Built %BUNDLE%\Max2Reframe.ofx
