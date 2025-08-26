@echo off
echo Creating projects...

:: Create required directories
if not exist "..\..\thirdparty" mkdir "..\..\thirdparty"
if not exist "..\..\thirdparty\libcurl" mkdir "..\..\thirdparty\libcurl"
if not exist "..\..\thirdparty\libcurl\lib" mkdir "..\..\thirdparty\libcurl\lib"
if not exist "..\..\thirdparty\libcurl\lib\win32" mkdir "..\..\thirdparty\libcurl\lib\win32"
if not exist "..\..\thirdparty\libcurl\include" mkdir "..\..\thirdparty\libcurl\include"
if not exist "..\..\thirdparty\libcurl\include\curl" mkdir "..\..\thirdparty\libcurl\include\curl"

:: Download libcurl
where curl >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    curl -L "https://github.com/curl/curl/releases/download/curl-7_58_0/curl-7.58.0.zip" -o "..\..\thirdparty\libcurl\curl.zip"
) else (
    echo Please download libcurl from: https://github.com/curl/curl/releases/download/curl-7_58_0/curl-7.58.0.zip
    echo Extract it and copy:
    echo   - include\curl\*.h to ..\..\thirdparty\libcurl\include\curl\
    echo   - lib\*.lib to ..\..\thirdparty\libcurl\lib\win32\
    echo Then press any key to continue...
    pause
)

:: Generate projects
devtools\bin\vpc.exe /hl2mp /tf /define:SOURCESDK ^
    +everything ^
    /mksln everything.sln

echo Project generation complete!
pause