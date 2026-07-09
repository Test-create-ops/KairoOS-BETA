@echo off
title Viteza OS
setlocal enabledelayedexpansion

echo ==========================================
echo   Viteza OS — Avvio in corso...
echo ==========================================
echo.

net session >nul 2>&1
set IS_ADMIN=%ERRORLEVEL%

where qemu-system-x86_64 >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [1/4] QEMU non trovato. Installo automaticamente...
    if %IS_ADMIN% EQU 0 (
        winget install QEMU 2>nul
        if !ERRORLEVEL! EQU 0 (
            echo QEMU installato!
        ) else (
            echo Installazione fallita. Scarica da: https://qemu.org/download
            start https://qemu.org/download
            pause
            exit /b
        )
    ) else (
        echo Esegui come amministratore per installazione automatica.
        start https://qemu.org/download
        pause
        exit /b
    )
) else (
    echo [1/4] QEMU trovato!
)

where ollama >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [2/4] Ollama non trovato. Scarico da ollama.com...
    powershell -Command "& {Invoke-WebRequest -Uri 'https://ollama.com/download/OllamaSetup.exe' -OutFile '%TEMP%\OllamaSetup.exe'}"
    if exist "%TEMP%\OllamaSetup.exe" (
        echo Installazione Ollama...
        start /wait "" "%TEMP%\OllamaSetup.exe" /S
        echo Ollama installato!
    ) else (
        start https://ollama.com/download
        pause
        exit /b
    )
) else (
    echo [2/4] Ollama trovato!
)

echo [3/4] Scarico modello AI (solo la prima volta)...
ollama pull llama3.2

echo [4/4] Avvio Viteza OS + AI...
echo.

start /B ollama serve
timeout /t 3 /nobreak >nul

start /B python social_proxy.py
timeout /t 2 /nobreak >nul

qemu-system-x86_64 -cdrom viteza.iso -m 256 -vga std ^
    -machine pc,accel=tcg -cpu qemu64 ^
    -serial tcp:localhost:9000,server,nowait

echo.
echo Viteza OS terminato. Ciao!
pause
