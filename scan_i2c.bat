@echo off
chcp 936 >nul
cd /d "%~dp0"
echo ================================================
echo   I2C bus scan
echo ================================================
echo   Healthy board : #I2C,count=2,a0=0x3D,a1=0x51
echo   0x51 = PCF8563 (RTC)
echo   + SGP30       : should also list 0x58
echo ================================================
echo.
python "tools\fish_tank_debugger.py" --cmd "I2C,SCAN" --port COM6 --wait 20
echo.
echo   Done. Press any key to close.
pause >nul
