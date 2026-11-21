#!/bin/sh
set -e
cd "$(dirname "$0")"
mkdir -p dist
cp FlashLaunch.exe dist/
cp README.md dist/
cd dist
python3 -c "import zipfile; z=zipfile.ZipFile('FlashLaunch-v1.9.zip','w',zipfile.ZIP_DEFLATED); [z.write(f) for f in ('FlashLaunch.exe','FlashLaunch.ini','README.md')]; z.close()"
cd ..
python3 -c "import zipfile,os; z=zipfile.ZipFile('dist/FlashLaunch-source.zip','w',zipfile.ZIP_DEFLATED); files=['build.sh','package.sh','README.md','src/FlashLaunch.cpp','src/pinyin_table.h','src/resource.rc','src/app.ico','tools/gen_pinyin.py','tools/gen_icon.py']; [z.write(f) for f in files]; z.close()"
ls -lh dist/
