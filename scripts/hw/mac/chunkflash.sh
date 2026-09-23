#!/bin/bash
# Chunked flash for the ESP32-C3 reader on this Mac.
# Usage: chunkflash.sh <factory-image> [port]
#
# Any sustained esptool write over ~3 s drops the USB link on this Mac, so the
# image is written in 128 KB chunks, each verified ("Hash of data verified")
# and retried up to 5 times. Do not simplify this into one write-flash call.
set -o pipefail
H=$HOME
IMG=${1:?usage: chunkflash.sh <image> [port]}
PORT=${2:-/dev/cu.usbmodem8401}
PY=$H/fwvenv/bin/python
WORK=$H/hw
mkdir -p "$WORK"
CHUNK=131072
SIZE=$(wc -c < "$IMG")
N=$(( (SIZE + CHUNK - 1) / CHUNK ))
echo "IMG $IMG size=$SIZE chunks=$N port=$PORT"
i=0
while [ $i -lt $N ]; do
  off=$(( i * CHUNK ))
  dd if="$IMG" of=$WORK/_chunk.bin bs=$CHUNK skip=$i count=1 2>/dev/null
  ok=0
  for try in 1 2 3 4 5; do
    $PY -m esptool --chip esp32c3 --port $PORT --baud 460800 --before usb-reset --after no-reset \
      write-flash --flash-size keep $off $WORK/_chunk.bin > $WORK/_c.log 2>&1
    if grep -q "Hash of data verified" $WORK/_c.log; then ok=1; break; fi
    sleep 1
  done
  if [ $ok -ne 1 ]; then echo "CHUNK $i @ $off FAILED after retries"; tail -3 $WORK/_c.log | tr -d "\r"; exit 1; fi
  echo "chunk $i/$N @ $(printf 0x%x $off) OK"
  i=$(( i + 1 ))
done
echo "ALL CHUNKS WRITTEN; resetting"
$PY -m esptool --chip esp32c3 --port $PORT --before usb-reset --after hard-reset chip-id > /dev/null 2>&1
echo "FLASH COMPLETE"
