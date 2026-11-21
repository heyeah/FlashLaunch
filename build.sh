#!/bin/sh
set -e
cd "$(dirname "$0")"

CXX=x86_64-w64-mingw32-g++
if ! command -v "$CXX" >/dev/null 2>&1 && [ -x "$PWD/.sysroot/usr/bin/x86_64-w64-mingw32-g++-win32" ]; then
    SYSROOT="$PWD/.sysroot"
    export PATH="$SYSROOT/usr/bin:$PATH"
    export LD_LIBRARY_PATH="$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/lib/x86_64-linux-gnu:$SYSROOT/usr/lib:$SYSROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    CXX="$SYSROOT/usr/bin/x86_64-w64-mingw32-g++-win32 --sysroot=$SYSROOT"
fi

if [ -n "$SYSROOT" ]; then
    ln -sf x86_64-w64-mingw32-gcc-win32 "$SYSROOT/usr/bin/x86_64-w64-mingw32-gcc"
    ln -sf x86_64-w64-mingw32-g++-win32 "$SYSROOT/usr/bin/x86_64-w64-mingw32-g++"
fi
if command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
    WR=x86_64-w64-mingw32-windres
else
    WR="$SYSROOT/usr/bin/x86_64-w64-mingw32-windres"
fi
(cd src && "$WR" -O coff -i resource.rc -o resource.res)

$CXX -std=c++17 -Os \
  -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti \
  -mwindows -municode -static-libgcc -static-libstdc++ \
  src/FlashLaunch.cpp src/resource.res \
  -o FlashLaunch.exe \
  -lcomctl32 -lshlwapi -lshell32 -lole32 -luuid -lgdiplus -ldwmapi -lgdi32 -luser32 -ladvapi32 \
  -Wl,--gc-sections -Wl,--strip-all
ls -lh FlashLaunch.exe
