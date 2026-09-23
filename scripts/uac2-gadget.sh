#!/bin/sh
# Runtime-only: add/remove a UAC2 audio function on Move's existing NCM gadget.
# Nothing here is persisted; a reboot restores the stock gadget.
set -e
G=/sys/kernel/config/usb_gadget/g1
F=$G/functions/uac2.0
CFG=$G/configs/c.1

CHANNELS=${CHANNELS:-10}
SRATE=${SRATE:-44100}
SSIZE=${SSIZE:-2}

case "$1" in
start)
  # 10 channels -> a 10-bit spatial channel mask.
  mask=$(awk "BEGIN{printf \"0x%x\", 2^$CHANNELS - 1}")

  echo "" > $G/UDC                       # unbind; this drops usb0
  mkdir -p $F
  echo $mask   > $F/p_chmask             # gadget -> host (what the Mac records)
  echo $SRATE  > $F/p_srate
  echo $SSIZE  > $F/p_ssize
  echo 0       > $F/c_chmask             # no host -> gadget stream
  echo 0       > $F/p_mute_present       # skip the control interrupt endpoint
  echo 0       > $F/p_volume_present

  # A composite NCM + UAC2 device must declare IAD, or a host binds only the
  # first function. This replaces bDeviceClass=2 (Communications).
  echo 0xEF > $G/bDeviceClass
  echo 0x02 > $G/bDeviceSubClass
  echo 0x01 > $G/bDeviceProtocol

  ln -sf $F $CFG/uac2.0
  ls /sys/class/udc > $G/UDC             # rebind
  echo "started: ${CHANNELS}ch @ ${SRATE} ${SSIZE}B, mask $mask"
  ;;
stop)
  echo "" > $G/UDC 2>/dev/null || true
  rm -f $CFG/uac2.0
  rmdir $F 2>/dev/null || true
  echo 2    > $G/bDeviceClass            # back to Communications
  echo 0    > $G/bDeviceSubClass
  echo 0    > $G/bDeviceProtocol
  ls /sys/class/udc > $G/UDC
  echo "stopped: stock NCM-only gadget restored"
  ;;
*) echo "usage: $0 start|stop" >&2; exit 2 ;;
esac
