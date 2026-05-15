@echo off
REM Flashes: custom bootloader + partition table + launcher to test partition
REM Usage: upload_factory.bat [COMx]
REM
REM The custom bootloader routes:
REM   Power on / reset button → test partition (launcher)
REM   esp_restart()           → ota_0 (user firmware)

set BOOTLOADER=.pio\build\bootloader\bootloader.bin
set PARTITIONS=.pio\build\m5cardputer\partitions.bin
set FIRMWARE=.pio\build\m5cardputer\firmware.bin

if not exist "%BOOTLOADER%" (
    echo Bootloader not built. Run: pio run -e bootloader
    exit /b 1
)
if not exist "%FIRMWARE%" (
    echo App not built. Run: pio run -e m5cardputer
    exit /b 1
)

set PORT=%1

echo Flashing custom bootloader + launcher...
echo   bootloader → 0x0
echo   partitions → 0x8000
echo   launcher   → 0x10000 (test partition)
echo.

if "%PORT%"=="" (
    python -m esptool --chip esp32s3 --baud 1500000 --before default_reset --after hard_reset write_flash -z 0x0 %BOOTLOADER% 0x8000 %PARTITIONS% 0x10000 %FIRMWARE%
) else (
    python -m esptool --chip esp32s3 --port %PORT% --baud 1500000 --before default_reset --after hard_reset write_flash -z 0x0 %BOOTLOADER% 0x8000 %PARTITIONS% 0x10000 %FIRMWARE%
)

if %errorlevel% == 0 (
    echo.
    echo Done.
    echo   Reset / power cycle → launcher
    echo   launch command      → user firmware
    echo   Reset again         → back to launcher
) else (
    echo.
    echo Flash failed.
)
pause
