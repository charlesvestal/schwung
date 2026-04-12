# USB-C Host Mode on Ableton Move

## Overview

The Move's USB-C port uses a DWC2 OTG controller (`fe980000.usb`) that normally runs in gadget mode, presenting USB ethernet (`usb0` / NCM) to the connected computer. It can be switched to host mode to connect USB devices — but the port does **not supply VBUS power**, so a **powered USB-C hub** is required.

## Requirements

- **Powered USB-C hub** (supplies its own 5V to downstream ports)
- **Root SSH access** (`ssh root@move.local`)
- **WiFi connection** to Move (USB ethernet will be lost during host mode)

## Hardware Details

- SoC: BCM2835-family (Pi CM4), ARM64
- USB controller: DWC2 at `fe980000.usb`, `dr_mode=otg`
- Kernel: 5.15.92-rt57-v8
- Built-in drivers: `usb-storage`, `uas`, `sd_mod`, `ext4`, `vfat`, `usbhid`
- No USB audio drivers (`snd-usb-audio` not available)

## Steps

### 1. Verify WiFi SSH works

```bash
ssh root@move.local 'echo ok'
```

All remaining commands go through WiFi — USB ethernet will be disabled.

### 2. Unbind the USB gadget

```bash
ssh root@move.local 'echo "" > /sys/kernel/config/usb_gadget/g1/UDC'
```

### 3. Switch to dwc_otg host driver

```bash
# Unbind dwc2 (gadget driver)
ssh root@move.local 'echo fe980000.usb > /sys/bus/platform/drivers/dwc2/unbind'

# Override to use the host driver
ssh root@move.local 'echo "dwc_otg" > /sys/devices/platform/soc/fe980000.usb/driver_override'

# Bind dwc_otg (host driver)
ssh root@move.local 'echo "fe980000.usb" > /sys/bus/platform/drivers_probe'
```

You should see in `dmesg`:
```
dwc_otg fe980000.usb: DWC OTG Controller
Init: Power Port (0)
hub 1-0:1.0: 1 port detected
```

### 4. Connect powered hub + device

Plug the powered USB-C hub into the Move's USB-C port, then connect your USB device to the hub. Check detection:

```bash
ssh root@move.local 'dmesg | tail -20'
ssh root@move.local 'lsusb'
```

### 5. Mount a USB drive (if applicable)

```bash
ssh root@move.local 'mkdir -p /data/UserData/usb && mount /dev/sda1 /data/UserData/usb'
```

Supported filesystems: ext4, vfat/FAT32, msdos. **Always mount under `/data/UserData/`** — never use `/tmp` or rootfs paths.

### 6. Restore gadget mode when done

```bash
# Unmount any drives first
ssh root@move.local 'umount /data/UserData/usb 2>/dev/null'

# Unbind dwc_otg
ssh root@move.local 'echo fe980000.usb > /sys/bus/platform/drivers/dwc_otg/unbind'

# Clear override
ssh root@move.local 'echo "" > /sys/devices/platform/soc/fe980000.usb/driver_override'

# Rebind dwc2
ssh root@move.local 'echo fe980000.usb > /sys/bus/platform/drivers/dwc2/bind'

# Restore gadget
ssh root@move.local 'echo fe980000.usb > /sys/kernel/config/usb_gadget/g1/UDC'
```

### If restoration fails

The `dwc_otg` driver is built into the kernel and may not release the memory region cleanly. If `dwc2` fails to bind with "can't request region", a **power cycle** (hold power button) is required. A soft reboot may not be sufficient.

## What works

- **USB mass storage** (flash drives, SSDs, HDDs) — drivers built-in
- **USB HID** (keyboards, controllers) — `usbhid` built-in
- **USB MIDI** — devices show up via standard USB MIDI class

## What doesn't work

- **USB audio interfaces** — no `snd-usb-audio` kernel module, no ALSA subsystem
- **Bus-powered devices** — USB-C port has no VBUS output; devices must be self-powered or powered through the hub

## Notes

- The USB-A port is **not** connected to the Pi's USB controller. It goes through the XMOS chip which only forwards MIDI over SPI. This cannot be changed without XMOS firmware modification.
- Move continues to function normally while in USB host mode (audio, pads, etc.), but the computer connection via USB-C is lost until gadget mode is restored.
