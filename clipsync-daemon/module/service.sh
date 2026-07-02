#!/system/bin/sh
MODDIR=${0%/*}
CONFIG="$MODDIR/config/clipsync.toml"

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done

if ! pidof clipsyncd > /dev/null 2>&1; then
    /data/adb/modules/clipsyncd/system/bin/clipsyncd --config "$CONFIG" &
fi
