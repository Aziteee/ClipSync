#!/system/bin/sh
MODDIR=${0%/*}

while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done

if ! pidof clipsyncd > /dev/null 2>&1; then
    /data/adb/modules/clipsyncd/clipsyncd &
fi
