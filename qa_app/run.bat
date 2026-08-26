@echo off
chcp 65001 >nul
echo.
echo ========================================
echo   C++ LLM Web 问答助手
echo ========================================
echo.

:: 运行程序
cd /d "%~dp0build"
qa_app.exe

pause
