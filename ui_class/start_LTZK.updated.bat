@echo off
setlocal

set "APP_DIR=%~dp0"
set "WUJI_ENGINE_DIR=%APP_DIR%plugins"

rem ===== PaddleOCR configuration =====
rem Change PADDLE_ENV_DIR only when the environment is installed elsewhere.
set "PADDLE_ENV_DIR=%APP_DIR%paddle_env"
set "PADDLE_PYTHON=%PADDLE_ENV_DIR%\Python311\python.exe"
set "PADDLE_OCR_SCRIPT=%PADDLE_ENV_DIR%\paddleocr_service.py"
set "PADDLE_PYTHONHOME=%PADDLE_ENV_DIR%\Python311"
set "PADDLE_PYTHONPATH=%PADDLE_ENV_DIR%\Lib\site-packages"
set "PADDLE_VENV_CFG=%PADDLE_ENV_DIR%\pyvenv.cfg"

if not exist "%PADDLE_PYTHON%" (
    echo PaddleOCR Python not found: "%PADDLE_PYTHON%"
    pause
    exit /b 1
)

if not exist "%PADDLE_OCR_SCRIPT%" (
    echo PaddleOCR service script not found: "%PADDLE_OCR_SCRIPT%"
    pause
    exit /b 1
)

rem Update the virtual-environment paths for the current installation folder.
(
    echo home = %PADDLE_PYTHONHOME%
    echo include-system-site-packages = false
    echo version = 3.11.9
    echo executable = %PADDLE_PYTHON%
    echo command = %PADDLE_PYTHON% -m venv %PADDLE_ENV_DIR%
) > "%PADDLE_VENV_CFG%"

set "PATH=%PADDLE_ENV_DIR%\Python311;%PADDLE_ENV_DIR%\Scripts;%PATH%"

if not exist "%APP_DIR%bin\LTZK.exe" (
    echo LTZK.exe not found: "%APP_DIR%bin\LTZK.exe"
    pause
    exit /b 1
)

if not exist "%APP_DIR%.env" (
    echo .env not found: "%APP_DIR%.env"
    echo Please edit .env and set QGIS_PREFIX_PATH and OSGEO4W_ROOT for this computer.
    pause
    exit /b 1
)

for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%APP_DIR%.env") do (
    if not "%%~A"=="" set "%%~A=%%~B"
)

if not defined QGIS_PREFIX_PATH (
    echo QGIS_PREFIX_PATH is not configured in "%APP_DIR%.env"
    pause
    exit /b 1
)

if not defined OSGEO4W_ROOT (
    echo OSGEO4W_ROOT is not configured in "%APP_DIR%.env"
    pause
    exit /b 1
)

set "QGIS_PREFIX_PATH=%QGIS_PREFIX_PATH:"=%"
set "OSGEO4W_ROOT=%OSGEO4W_ROOT:"=%"

if not "%QGIS_PREFIX_PATH:~1,1%"==":" if not "%QGIS_PREFIX_PATH:~0,2%"=="\\" set "QGIS_PREFIX_PATH=%APP_DIR%%QGIS_PREFIX_PATH%"
if not "%OSGEO4W_ROOT:~1,1%"==":" if not "%OSGEO4W_ROOT:~0,2%"=="\\" set "OSGEO4W_ROOT=%APP_DIR%%OSGEO4W_ROOT%"

for %%I in ("%QGIS_PREFIX_PATH%") do set "QGIS_PREFIX_PATH=%%~fI"
for %%I in ("%OSGEO4W_ROOT%") do set "OSGEO4W_ROOT=%%~fI"

if not exist "%QGIS_PREFIX_PATH%" (
    echo QGIS prefix path not found: "%QGIS_PREFIX_PATH%"
    pause
    exit /b 1
)

if not exist "%OSGEO4W_ROOT%" (
    echo OSGeo4W root not found: "%OSGEO4W_ROOT%"
    pause
    exit /b 1
)

if not exist "%QGIS_PREFIX_PATH%\bin" (
    echo QGIS bin folder not found: "%QGIS_PREFIX_PATH%\bin"
    pause
    exit /b 1
)

if not exist "%OSGEO4W_ROOT%\bin" (
    echo OSGeo4W bin folder not found: "%OSGEO4W_ROOT%\bin"
    pause
    exit /b 1
)

if not exist "%OSGEO4W_ROOT%\apps\Qt5\bin" (
    echo Qt bin folder not found: "%OSGEO4W_ROOT%\apps\Qt5\bin"
    pause
    exit /b 1
)

if not exist "%OSGEO4W_ROOT%\apps\Qt5\plugins\platforms" (
    echo Qt platform plugins folder not found: "%OSGEO4W_ROOT%\apps\Qt5\plugins\platforms"
    pause
    exit /b 1
)

set "PATH=%APP_DIR%bin;%QGIS_PREFIX_PATH%\bin;%OSGEO4W_ROOT%\bin;%OSGEO4W_ROOT%\apps\Qt5\bin;%PATH%"
set "QT_PLUGIN_PATH=%OSGEO4W_ROOT%\apps\Qt5\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%OSGEO4W_ROOT%\apps\Qt5\plugins\platforms"
set "GDAL_DATA=%OSGEO4W_ROOT%\share\gdal"
set "PROJ_LIB=%OSGEO4W_ROOT%\share\proj"
set "QGIS_PLUGINPATH=%QGIS_PREFIX_PATH%\plugins"
set "PYTHONHOME=%OSGEO4W_ROOT%\apps\Python39"
set "PYTHONPATH="
set "PYTHONUTF8=1"
set "PATH=%OSGEO4W_ROOT%\apps\Python39\Scripts;%PATH%"

cd /d "%APP_DIR%bin"
start "" "%APP_DIR%bin\LTZK.exe"
