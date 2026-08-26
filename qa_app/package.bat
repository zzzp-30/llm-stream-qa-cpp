@echo off
chcp 65001 >nul 2>&1
echo ========================================
echo   AI Assistant - 打包部署脚本
echo ========================================
echo.

set BUILD_DIR=%~dp0build
set DEPLOY_DIR=%~dp0deploy
set MSYS=C:\msys64\ucrt64\bin

:: 清理并创建部署目录
if exist "%DEPLOY_DIR%" rmdir /s /q "%DEPLOY_DIR%"
mkdir "%DEPLOY_DIR%\web"

:: 复制主程序
echo [1/4] 复制主程序...
copy "%BUILD_DIR%\qa_app.exe" "%DEPLOY_DIR%\" >nul

:: 复制 web 前端
echo [2/4] 复制 Web 前端...
xcopy "%BUILD_DIR%\web\*" "%DEPLOY_DIR%\web\" /E /Y /Q >nul

:: 复制配置文件
echo [3/4] 复制配置文件...
if exist "%BUILD_DIR%\config.txt" (
    copy "%BUILD_DIR%\config.txt" "%DEPLOY_DIR%\" >nul
) else (
    echo # AI Assistant 配置文件> "%DEPLOY_DIR%\config.txt"
    echo api_url=https://api.openai.com/v1>> "%DEPLOY_DIR%\config.txt"
    echo api_key=YOUR_KEY_HERE>> "%DEPLOY_DIR%\config.txt"
    echo model=gpt-3.5-turbo>> "%DEPLOY_DIR%\config.txt"
    echo vision_model=gpt-4o>> "%DEPLOY_DIR%\config.txt"
    echo     已创建示例 config.txt，请填写 API Key
)

:: 复制依赖 DLL（使用通配符匹配，不怕版本号变化）
echo [4/4] 复制依赖 DLL...
for %%f in (
    libcurl-4*.dll
    libcrypto-3*.dll
    libssl-3*.dll
    libgcc_s_seh-1*.dll
    libstdc++-6*.dll
    libwinpthread-1*.dll
    libiconv-2*.dll
    libintl-8*.dll
    libidn2-0*.dll
    libunistring*.dll
    libbrotlicommon*.dll
    libbrotlidec*.dll
    libnghttp2-14*.dll
    libnghttp3*.dll
    libngtcp2-16*.dll
    libngtcp2_crypto_ossl*.dll
    libpsl-5*.dll
    libssh2-1*.dll
    libzstd*.dll
    zlib1*.dll
) do (
    if exist "%MSYS%\%%f" (
        copy "%MSYS%\%%f" "%DEPLOY_DIR%\" >nul
    )
)

echo.
echo ========================================
echo   打包完成！
echo   输出目录: %DEPLOY_DIR%
echo.
echo   部署方式: 将整个 deploy 文件夹复制到
echo   任意位置，双击 qa_app.exe 即可运行。
echo   首次使用请先编辑 config.txt 填写 API Key。
echo ========================================
pause
