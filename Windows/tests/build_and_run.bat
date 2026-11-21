@echo off
rem Headless tests for the PC side. They use YOUR FFmpeg (shared build with
rem include\ and lib\), not the prebuilt libraries in Windows\external.
rem   build_and_run.bat C:\path\to\ffmpeg
setlocal
if "%~1"=="" (echo usage: build_and_run.bat FFMPEG_DIR & exit /b 2)
set FF=%~1
set SRC=%~dp0..\EncomService
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%~dp0out"
set CL=/nologo /EHsc /O2 /std:c++17 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%SRC%" /I "%FF%\include"
set LIBS=/link /LIBPATH:"%FF%\lib" avformat.lib avcodec.lib avutil.lib swscale.lib advapi32.lib shell32.lib
cl ..\test_shm.cpp /Fe:test_shm.exe /link advapi32.lib || exit /b 1
cl ..\test_pipeline.cpp "%SRC%\VideoReceiver.cpp" /Fe:test_pipeline.exe %LIBS% || exit /b 1
cl /DOPENCAMERA_LOOPBACK_TEST ..\test_link.cpp "%SRC%\PhoneLink.cpp" "%SRC%\AppState.cpp" "%SRC%\VideoReceiver.cpp" /Fe:test_link.exe %LIBS% || exit /b 1
set PATH=%FF%\bin;%PATH%
"%FF%\bin\ffmpeg.exe" -hide_banner -loglevel error -y -f lavfi -i "color=c=0x2050C0:s=1280x720:r=30:d=4,drawbox=x=0:y=0:w=iw:h=40:color=white:t=fill" -c:v libx264 -pix_fmt yuv420p -g 30 t720.mp4
"%FF%\bin\ffmpeg.exe" -hide_banner -loglevel error -y -f lavfi -i "color=c=0x2050C0:s=1920x1080:r=30:d=4,drawbox=x=0:y=0:w=iw:h=60:color=white:t=fill" -c:v libx264 -pix_fmt yuvj420p -color_range pc -g 30 t1080j.mp4
echo == shared memory stress
test_shm.exe
echo == decode 720p CPU / GPU, 1080p full-range CPU / GPU
test_pipeline.exe t720.mp4 0 192 80 32
test_pipeline.exe t720.mp4 1 192 80 32
test_pipeline.exe t1080j.mp4 0 192 80 32
test_pipeline.exe t1080j.mp4 1 192 80 32
echo == phone link (loopback only)
start /b test_link.exe 24
timeout /t 2 >nul
python -I ..\fake_phone.py
timeout /t 6 >nul
