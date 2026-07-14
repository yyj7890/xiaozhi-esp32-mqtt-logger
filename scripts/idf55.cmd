@echo off
setlocal

rem Command-line ESP-IDF v5.5.4 environment installed from the offline archive.
set "IDF_PATH=D:\Espressif\v5.5.4\esp-idf"
set "IDF_TOOLS_PATH=D:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\tools\python\v5.5.4\venv"
set "PATH=D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;D:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\idf-exe\1.0.3;%PATH%"

"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" %*
exit /b %ERRORLEVEL%
