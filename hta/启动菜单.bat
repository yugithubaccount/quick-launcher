@echo off
chcp 936 >nul
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "LIST=%ROOT%tools_gbk.txt"

:search
cls
echo ==========================================================
echo    取证应急工具箱  ^|  命令行菜单（免安装 / 换电脑可用）
echo ==========================================================
echo.
if not exist "%LIST%" echo [错误] 找不到 tools_gbk.txt & pause & exit /b 1
set "kw="
set /p "kw=输入关键词搜索（名称或路径，直接回车=列出全部）: "
if "!kw!"=="" (findstr /n "." "%LIST%" > "%TEMP%\tb_res.txt") else (findstr /i /c:"!kw!" "%LIST%" > "%TEMP%\tb_res.txt")
set n=0
for /f "usebackq tokens=1,2,3 delims=|" %%a in ("%TEMP%\tb_res.txt") do (
  set /a n+=1
  set "pt!n!=%%c"
  set "nm!n!=%%a / %%b"
  echo   !n!^) %%a / %%b
  echo        %%c
)
echo.
if !n!==0 (echo   没有匹配的条目 & echo. & pause & goto search)
echo   共 !n! 项
echo.
set "sel="
set /p "sel=输入序号启动 [0=重新搜索  q=退出]: "
if /i "!sel!"=="q" goto :eof
if "!sel!"=="0" goto search
if "!sel!"=="" goto search
for /f "delims=0123456789" %%z in ("!sel!") do (echo   请输入数字序号 & pause & goto search)
set "p=!pt%sel%!"
if "!p!"=="" (echo   序号超出范围 & pause & goto search)
set "f=%ROOT%!p!"
if exist "!f!" (
  echo.
  echo   正在启动: !nm%sel%!
  if /i "!f:~-4!"==".ps1" (start "" powershell -NoProfile -ExecutionPolicy Bypass -File "!f!") else (start "" "!f!")
) else (
  echo.
  echo   [提示] 文件不存在: !f!
  echo   可到工具箱根目录核对是否已复制过来。
  pause
)
goto search
