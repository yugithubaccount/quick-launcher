#!/bin/bash
# 交叉编译 Windows 便携快速启动器（Debian/Kali 上使用 mingw-w64）
# 产物: 便携启动器/便携快速启动器.exe (32位, 兼容性最好)  与  便携快速启动器_x64.exe
set -e
cd "$(dirname "$0")"
CC32=i686-w64-mingw32-gcc
CC64=x86_64-w64-mingw32-gcc
RC32=i686-w64-mingw32-windres
RC64=x86_64-w64-mingw32-windres
CFLAGS="-municode -mwindows -O2 -s -static -finput-charset=UTF-8"
LIBS="-lcomctl32 -lshlwapi -lshell32 -luser32 -lgdi32 -lcomdlg32"

python3 make_icon.py app.ico                     # 生成图标
$RC32 -c 65001 app.rc app_res32.o                # 资源(图标/清单/版本信息)
$RC64 -c 65001 app.rc app_res64.o
$CC32 $CFLAGS 便携快速启动器.c app_res32.o -o ../dist/便携快速启动器.exe $LIBS
$CC64 $CFLAGS 便携快速启动器.c app_res64.o -o ../dist/便携快速启动器_x64.exe $LIBS
echo "构建完成:"; ls -la ../dist/*.exe
