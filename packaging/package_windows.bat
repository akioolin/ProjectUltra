@echo off
REM Build the Windows alpha operator bundle and separate developer-tools bundle.
REM Run this from a Visual Studio Developer Command Prompt.

set TARGET=windows
set OUTPUT_DIR=dist\windows
set OPERATOR_DIR=%OUTPUT_DIR%\projectultra-%TARGET%
set DEV_DIR=%OUTPUT_DIR%\dev-tools-%TARGET%

echo === Packaging ProjectUltra operator bundle for Windows ===

cd ..
if not exist build-release mkdir build-release
cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release -DULTRA_BUILD_TESTS=OFF -DULTRA_BUILD_GUI=ON
cmake --build . --config Release --target ultra ultra_tnc ultra_gui cli_simulator threaded_simulator test_waveform_simple decode_bench session_decode
cd ..\packaging

if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OPERATOR_DIR%"
mkdir "%OPERATOR_DIR%\tools"
mkdir "%OPERATOR_DIR%\docs"
mkdir "%DEV_DIR%"

copy "..\build-release\Release\ultra.exe" "%OPERATOR_DIR%\"
copy "..\build-release\Release\ultra_tnc.exe" "%OPERATOR_DIR%\"
copy "..\build-release\Release\ultra_gui.exe" "%OPERATOR_DIR%\"
copy "..\tools\ultra_tnc.conf.example" "%OPERATOR_DIR%\tools\"
copy "..\README.md" "%OPERATOR_DIR%\"
copy "..\docs\TNC_INTERFACE.md" "%OPERATOR_DIR%\docs\"
copy "..\docs\README.md" "%OPERATOR_DIR%\docs\"
if exist "..\docs\RUNNING.md" copy "..\docs\RUNNING.md" "%OPERATOR_DIR%\RUNNING.md"

copy "..\build-release\Release\cli_simulator.exe" "%DEV_DIR%\" 2>nul
copy "..\build-release\Release\threaded_simulator.exe" "%DEV_DIR%\" 2>nul
copy "..\build-release\Release\test_waveform_simple.exe" "%DEV_DIR%\" 2>nul
copy "..\build-release\Release\decode_bench.exe" "%DEV_DIR%\" 2>nul
copy "..\build-release\Release\session_decode.exe" "%DEV_DIR%\" 2>nul

if exist "%VCPKG_ROOT%\installed\x64-windows\bin\SDL2.dll" (
    copy "%VCPKG_ROOT%\installed\x64-windows\bin\SDL2.dll" "%OPERATOR_DIR%\"
    copy "%VCPKG_ROOT%\installed\x64-windows\bin\SDL2.dll" "%DEV_DIR%\"
) else if exist "C:\SDL2\lib\x64\SDL2.dll" (
    copy "C:\SDL2\lib\x64\SDL2.dll" "%OPERATOR_DIR%\"
    copy "C:\SDL2\lib\x64\SDL2.dll" "%DEV_DIR%\"
) else (
    echo ERROR: SDL2.dll not found in VCPKG_ROOT or C:\SDL2\lib\x64
    exit /b 1
)

echo ProjectUltra alpha operator bundle > "%OPERATOR_DIR%\BUNDLE.txt"
echo Includes ultra_tnc, ultra_gui, ultra, sample config, and operator docs. >> "%OPERATOR_DIR%\BUNDLE.txt"
echo Developer simulators and bench tools are packaged separately. >> "%OPERATOR_DIR%\BUNDLE.txt"

echo ProjectUltra developer tools > "%DEV_DIR%\BUNDLE.txt"
echo This artifact is not the default operator download. >> "%DEV_DIR%\BUNDLE.txt"

powershell -Command "Compress-Archive -Path '%OPERATOR_DIR%\*' -DestinationPath '%OUTPUT_DIR%\projectultra-windows.zip' -Force"
powershell -Command "Compress-Archive -Path '%DEV_DIR%\*' -DestinationPath '%OUTPUT_DIR%\dev-tools-windows.zip' -Force"

echo.
echo === Windows Bundles Complete ===
echo Operator bundle: %OUTPUT_DIR%\projectultra-windows.zip
echo Developer tools: %OUTPUT_DIR%\dev-tools-windows.zip
