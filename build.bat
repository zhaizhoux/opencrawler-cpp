@echo off
rem OpenCrawlerCpp Windows 一键构建（MinGW-w64 + Ninja）
setlocal
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo.
echo 构建完成：build\opencrawler-cpp.exe
echo 自测：build\opencrawler-cpp.exe selftest
