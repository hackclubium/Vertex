@echo off
setlocal enabledelayedexpansion
set PROBE=C:\Users\manoj\StarDance\Vertex\build\Debug\site_probe.exe
set OUTFILE=C:\Users\manoj\StarDance\Vertex\tools\site_probe_results.txt
echo === SITE_PROBE TOP 20 RESULTS === > %OUTFILE%
echo Timestamp: %date% %time% >> %OUTFILE%
echo. >> %OUTFILE%

set URLS=google.com youtube.com facebook.com twitter.com instagram.com baidu.com wikipedia.org yahoo.com yandex.com whatsapp.com xvideos.com amazon.com pornhub.com live.com tiktok.com reddit.com docomo.ne.jp linkedin.com office.com netflix.com

set COUNT=0
for %%u in (%URLS%) do (
    set /a COUNT+=1
    echo.
    echo === [!COUNT!/20] %%u ===
    echo === [!COUNT!/20] %%u === >> %OUTFILE%
    "%PROBE%" "%%u" 32 2>&1 >> %OUTFILE%
    if !errorlevel! neq 0 (
        echo EXIT_CODE=!errorlevel! >> %OUTFILE%
    )
    echo. >> %OUTFILE%
)

echo.
echo === DONE ===
echo Results saved to %OUTFILE%
