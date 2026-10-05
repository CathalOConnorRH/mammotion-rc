# Mammotion Remote Home Assistant Add-on

This add-on runs the mammotion-rc web server inside Home Assistant.

## Requirements

- Home Assistant OS or Supervised
- amd64 or aarch64 (e.g. Raspberry Pi 4/5, Home Assistant Green/Yellow)
- Network access to the mower and, if used, the HC33 camera board
- A valid mowers.toml and secrets.toml

## Installation

Add this repository to Home Assistant as a custom add-on repository:

https://github.com/ontheview/mammotion-rc

Then install Mammotion Remote from the Add-on Store.

## Web interface

The web interface is exposed on:

https://<home-assistant-host>:8443/

The add-on uses host networking.

## Configuration files

Runtime configuration is stored under:

/data/mammotion-rc

The following files are expected there:

mowers.toml
secrets.toml

## HC33 camera support

If the HC33 camera integration is installed, configure the camera host in the mower configuration.

Example:

hc33_host = "192.168.1.175"

Replace the example address with the actual IP address of your HC33 board.

The web server proxies the HC33 MJPEG stream so it can be viewed through the HTTPS interface.

Current HC33 firmware limitation: 1 concurrent MJPEG stream client.

## LoRa mowers (Heltec V3)

For the LoRa variant (see the main README), plug the **base** V3 board into the
Home Assistant machine by USB. The add-on then runs `lora_bridge.py` alongside
the web server.

1. Plug in the base board, then open the add-on's **Configuration** tab.
2. Set `lora_serial` to the board. It's listed as
   `/dev/serial/by-id/usb-Silicon_Labs_CP2102...`. Use the `by-id` path, not
   `/dev/ttyUSB0`, so the setting survives reboots and other USB devices.
3. Set `lora_mower` to the mower's Mammotion device name (e.g.
   `Luba-XXXXXXXX`, as shown in the Mammotion app's device info). The add-on
   writes its `mowers.toml` entry (`127.0.0.1:9876`, `link = "lora"`) on
   start, and keeps any other mowers already listed. Onboarding's network scan
   can't find LoRa mowers, so this replaces it.
4. Save and restart the add-on. The log shows `mowers.toml: added LoRa mower
   ...` and `Starting LoRa bridge on ...`, followed by `base: mammotion-rc
   v3-base ...` once the board answers.

With a mower configured this way, the web UI skips onboarding and the
Mammotion account login. Driving doesn't need the login; only the camera does,
and the camera doesn't work over LoRa anyway.

The Home Assistant machine is now one end of the radio link, so it has to be
within LoRa range of the mower. A USB extension cable lets you put the base
board near a window or up high.

Leave `lora_serial` empty if you only have HC33 (Wi-Fi/HaLow) mowers.

The image clones the web server from the repo and branch set in `build.yaml`.
After new commits land there, use **Rebuild** on the add-on page to pick them up.

## HTTPS certificate

If no certificate exists, the add-on generates a local certificate automatically.

The files cert.pem and key.pem are stored in /data/mammotion-rc.
