@echo off
REM Build script for Fleaux WASM coordinator (Windows)
REM Usage: build_wasm.bat [clean]

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..\..
set CORE_DIR=%PROJECT_ROOT%\core
set WASM_DIR=%SCRIPT_DIR%

set CLEAN_BUILD=0
if "%1"=="clean" set CLEAN_BUILD=1

echo === Fleaux WASM Coordinator Build (Windows) ===
echo.

REM Step 1: Verify Emscripten is available
echo [1/4] Verifying Emscripten SDK...
where emcc >nul 2>&1
if errorlevel 1 (
    echo Error: emcc not found. Please install and activate Emscripten SDK.
    echo Visit: https://emscripten.org/docs/getting_started/downloads.html
    exit /b 1
)
for /f "tokens=*" %%i in ('emcc --version ^| findstr /R "."') do set EMCC_VERSION=%%i
echo OK Found: !EMCC_VERSION!
echo.

REM Step 2: Verify Conan is available
echo [2/4] Verifying Conan package manager...
where conan >nul 2>&1
if errorlevel 1 (
    echo Error: conan not found. Please install Conan 2.0 or later.
    echo Visit: https://conan.io/download
    exit /b 1
)
for /f "tokens=*" %%i in ('conan --version') do set CONAN_VERSION=%%i
echo OK Found: !CONAN_VERSION!
echo.

REM Step 3: Clean if requested
if %CLEAN_BUILD%==1 (
    echo [3/4] Cleaning previous build...
    if exist "%CORE_DIR%\cmake-build-wasm" rmdir /s /q "%CORE_DIR%\cmake-build-wasm"
    if exist "%CORE_DIR%\conan_build_wasm" rmdir /s /q "%CORE_DIR%\conan_build_wasm"
    echo OK Clean complete
    echo.
) else (
    echo [3/4] Installing Conan dependencies...
    cd /d "%CORE_DIR%"

    echo Running: conan install . --profile=emscripten_wasm -o build_wasm_coordinator=True --build=missing
    call conan install . --profile=emscripten_wasm -o build_wasm_coordinator=True --build=missing
    if errorlevel 1 (
        echo Error: Conan install failed
        exit /b 1
    )
    echo OK Dependencies installed
    echo.
)

REM Step 4: Build with CMake
echo [4/4] Building WASM coordinator...
cd /d "%CORE_DIR%"

if exist "CMakePresets.json" (
    echo Using CMake preset: conan-release
    cmake --preset conan-release -DFLEAUX_BUILD_WASM_COORDINATOR=ON
    if errorlevel 1 (
        echo Error: CMake configuration failed
        exit /b 1
    )
    cmake --build --preset conan-release
    if errorlevel 1 (
        echo Error: CMake build failed
        exit /b 1
    )
) else (
    echo Configuring with Emscripten toolchain...
    if not exist "cmake-build-wasm" mkdir cmake-build-wasm
    cd /d cmake-build-wasm

    cmake .. ^
        -DCMAKE_TOOLCHAIN_FILE="!EMSDK!\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake" ^
        -DFLEAUX_BUILD_WASM_COORDINATOR=ON ^
        -DCMAKE_BUILD_TYPE=Release

    if errorlevel 1 (
        echo Error: CMake configuration failed
        exit /b 1
    )

    echo Building...
    cmake --build . --config Release
    if errorlevel 1 (
        echo Error: CMake build failed
        exit /b 1
    )
)

REM Verify output
set OUTPUT_DIR=%PROJECT_ROOT%\fleaux-visual\public\wasm
if exist "%OUTPUT_DIR%\fleaux_wasm_coordinator.js" if exist "%OUTPUT_DIR%\fleaux_wasm_coordinator.wasm" (
    echo.
    echo OK Build successful!
    echo  JavaScript: %OUTPUT_DIR%\fleaux_wasm_coordinator.js
    echo  WebAssembly: %OUTPUT_DIR%\fleaux_wasm_coordinator.wasm
    echo.
    echo Next steps:
    echo  1. Test the WASM module:
    echo     npm test  ^(from fleaux-visual directory^)
    echo  2. Run the development server:
    echo     npm run dev  ^(from fleaux-visual directory^)
    exit /b 0
) else (
    echo Error: Build failed - Output files not found
    echo  Expected: %OUTPUT_DIR%\fleaux_wasm_coordinator.js
    echo  Expected: %OUTPUT_DIR%\fleaux_wasm_coordinator.wasm
    exit /b 1
)

endlocal


