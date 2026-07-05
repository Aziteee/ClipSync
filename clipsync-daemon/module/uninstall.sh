#!/system/bin/sh
MODDIR=${0%/*}
PERSIST_DIR="/data/adb/clipsyncd"

pidof clipsyncd > /dev/null 2>&1 && killall clipsyncd

rm -rf "$PERSIST_DIR"
