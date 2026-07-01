#!/system/bin/sh
MODDIR=${0%/*}

# Only start daemon after boot completes
(sleep 30 && \
  nohup $MODDIR/system/bin/clipsyncd \
    --port 5287 \
    --secret "$(cat $MODDIR/config/clipsync.toml | grep secret | cut -d'"' -f2)" \
    > /data/local/tmp/clipsyncd.log 2>&1 &
) &
