#!/system/bin/sh
MODDIR=${0%/*}
PERSIST_DIR="/data/adb/clipsyncd"
PERSIST_CONFIG="$PERSIST_DIR/clipsync.toml"
DEFAULT_CONFIG="$MODDIR/config/clipsync.toml"

if [ ! -f "$PERSIST_CONFIG" ]; then
    mkdir -p "$PERSIST_DIR"
    cp "$DEFAULT_CONFIG" "$PERSIST_CONFIG"
fi

pidof clipsyncd > /dev/null 2>&1 && killall clipsyncd
sleep 1
"$MODDIR/system/bin/clipsyncd" --config "$PERSIST_CONFIG" &

echo "clipsyncd restarted"
