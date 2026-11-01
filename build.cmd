@echo off
rem MouseBattery 构建脚本
rem 需要 PATH 中可用的 gcc(MinGW-w64 / 32 位 MinGW);
rem 也可以用 GCC_DIR 指定编译器的 bin 目录,例如:
rem   set GCC_DIR=D:\Soft\Dev-Cpp\MinGW32\bin
setlocal
if not "%GCC_DIR%"=="" set PATH=%GCC_DIR%;%PATH%

gcc -O2 -mwindows -municode -o MouseBattery.exe mouse_battery.c -lsetupapi -lhid -lgdi32
if errorlevel 1 (echo [FAIL] MouseBattery.exe & exit /b 1)

gcc -O2 -mconsole -municode -o test_query.exe test_query.c -lsetupapi -lhid
if errorlevel 1 (echo [FAIL] test_query.exe & exit /b 1)

echo [OK] MouseBattery.exe / test_query.exe
