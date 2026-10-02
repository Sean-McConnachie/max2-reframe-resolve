@echo off
rem Builds the OFX plugin bundle and the max2render test tool into build\
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not defined CUDA_PATH set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.3"

set OUT=build
set OBJ=build\obj
set GEN=build\gen
if not exist %OBJ%\sdk mkdir %OBJ%\sdk
if not exist %OBJ%\core mkdir %OBJ%\core

set CFLAGS=/nologo /O2 /EHsc /MD /std:c++17 /W3 /DWIN32 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Ithird_party\openfx\include /Ithird_party\openfx\Support\include /Isrc /I%GEN% /I"%CUDA_PATH%\include"
set LIBS=mfplat.lib mfreadwrite.lib mfuuid.lib d3d11.lib dxgi.lib ole32.lib propsys.lib user32.lib shell32.lib "%CUDA_PATH%\lib\x64\cudart_static.lib" "%CUDA_PATH%\lib\x64\OpenCL.lib"

rem OpenFX support library (only rebuilt when missing)
set SDK=third_party\openfx\Support\Library
if not exist %OBJ%\sdk\ofxsImageEffect.obj (
  cl %CFLAGS% /c %SDK%\ofxsCore.cpp %SDK%\ofxsImageEffect.cpp %SDK%\ofxsInteract.cpp %SDK%\ofxsLog.cpp %SDK%\ofxsMultiThread.cpp %SDK%\ofxsParams.cpp %SDK%\ofxsProperty.cpp %SDK%\ofxsPropertyValidation.cpp /Fo%OBJ%\sdk\ || exit /b 1
)

rem OpenCL kernel source embedded as a string
powershell -NoProfile -ExecutionPolicy Bypass -File tools\embed.ps1 -In src\reframe_kernel.h -Out %GEN%\reframe_kernel_cl.inc || exit /b 1

rem CUDA kernels: native code for Turing/Ampere/Ada plus PTX for newer GPUs
"%CUDA_PATH%\bin\nvcc.exe" -c src\gpu_cuda.cu -o %OBJ%\core\gpu_cuda.obj -O3 -std=c++17 -allow-unsupported-compiler -Xcompiler "/MD /EHsc /nologo" -Isrc -gencode arch=compute_75,code=sm_75 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 -gencode arch=compute_89,code=compute_89 || exit /b 1

cl %CFLAGS% /c src\*.cpp /Fo%OBJ%\core\ || exit /b 1

set CORE=%OBJ%\core\mp4.obj %OBJ%\core\source360.obj %OBJ%\core\decoder.obj %OBJ%\core\reproject.obj %OBJ%\core\engine.obj %OBJ%\core\log.obj %OBJ%\core\gpu_opencl.obj %OBJ%\core\gpu_cuda.obj

cl %CFLAGS% tools\max2render.cpp %CORE% /Fo%OBJ%\ /Fe%OUT%\max2render.exe /link %LIBS% || exit /b 1

rem The real plugin (installed to %LOCALAPPDATA%\Max2Reframe\core) ...
link /nologo /DLL /OUT:%OUT%\Max2ReframeCore.dll %OBJ%\core\plugin.obj %CORE% %OBJ%\sdk\*.obj %LIBS% || exit /b 1

rem ... and the small loader that sits in Resolve's plugin folder and forwards to it
set BUNDLE=%OUT%\Max2Reframe.ofx.bundle\Contents\Win64
if not exist %BUNDLE% mkdir %BUNDLE%
cl %CFLAGS% /c src\loader\loader.cpp /Fo%OBJ%\loader.obj || exit /b 1
link /nologo /DLL /OUT:%BUNDLE%\Max2Reframe.ofx %OBJ%\loader.obj shell32.lib ole32.lib || exit /b 1
echo Built %OUT%\Max2ReframeCore.dll and %BUNDLE%\Max2Reframe.ofx
