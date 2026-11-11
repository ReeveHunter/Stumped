#!/bin/sh
# Onion OS launcher for Stumped
cd "$(dirname "$0")"
miyoodir=/mnt/SDCARD/miyoo
# Use Onion's own SDL build (same library path Onion gives its built-in apps)
export LD_LIBRARY_PATH="/lib:/config/lib:$miyoodir/lib:/mnt/SDCARD/.tmp_update/lib:$(pwd)/lib:$LD_LIBRARY_PATH"
# Route audio through Onion's audioserver, as Onion does for RetroArch and games
[ -f "$miyoodir/lib/libpadsp.so" ] && export LD_PRELOAD="$miyoodir/lib/libpadsp.so"
./stumped > log.txt 2>&1
