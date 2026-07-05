#!/system/bin/sh
MODDIR=${0%/*}
HELPER_SRC="$MODDIR/clipsync-helper.jar"
HELPER_DST="/data/system/clipsync-helper.jar"

if [ -f "$HELPER_SRC" ]; then
    cp "$HELPER_SRC" "$HELPER_DST"
    chown system:system "$HELPER_DST"
    chmod 0644 "$HELPER_DST"
    restorecon "$HELPER_DST" 2>/dev/null
fi
