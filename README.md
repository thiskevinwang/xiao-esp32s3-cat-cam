# XIAO ESP32S3 Cat Cam

ESP-IDF firmware for the XIAO ESP32S3 Sense. The USB-C connection supplies power and exposes a standard macOS UVC webcam named **XIAO ESP32S3 Cat Cam**. Each 320×240 frame runs through Espressif's ESPDet-Pico cat model. Detected cats get a green bounding box before MJPEG encoding.

## Requirements

- XIAO ESP32S3 Sense with its camera board attached
- Data-capable USB-C cable
- ESP-IDF v5.4.4
- macOS accessory access for the Espressif device

The firmware supports the OV2640, OV3660, and OV5640 camera modules supported by the XIAO Sense camera board.

## Build and flash

Install ESP-IDF once:

```sh
git clone --branch v5.4.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

Build and flash from this repository:

```sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

The serial device disconnects after boot because the USB port changes from the ROM serial/JTAG device to the UVC camera.

## FAQ

### How do I test the video input?

1. Accept the macOS accessory prompt when it appears.
2. Open QuickTime Player.
3. Select **File → New Movie Recording**.
4. Open the camera menu next to the record button.
5. Select **UVC CAM1**. macOS shows the USB product as **XIAO ESP32S3 Cat Cam**.

Point the camera at a cat or a clear cat photo. The video shows a green box around each detected cat. Expected output is about 5 FPS.

If no video appears, disconnect and reconnect USB-C. Accept the macOS accessory prompt, then select **UVC CAM1** again.

Other test methods:

- **Photo Booth:** Open Photo Booth, select **Camera → UVC CAM1**, and check the preview.
- **OBS Studio:** Add a **Video Capture Device** source. Select **UVC CAM1**, 320×240, and 5 FPS.
- **FFmpeg:** List the available cameras, then save a 10-second test clip:

  ```sh
  ffmpeg -f avfoundation -list_devices true -i ""
  ffmpeg -f avfoundation -framerate 5 -video_size 320x240 \
    -i "UVC CAM1:none" -t 10 xiao-test.mov
  ```

See [Apple's external camera instructions](https://support.apple.com/guide/mac-help/choose-an-external-camera-mchl034033f4/mac), the [OBS video capture guide](https://obsproject.com/kb/video-capture-sources), and the [FFmpeg AVFoundation documentation](https://www.ffmpeg.org/ffmpeg-devices.html#avfoundation).

### Can I view the video without saving a clip?

Yes. QuickTime Player, Photo Booth, and OBS show a continuous preview without recording. In QuickTime Player, do not click the record button.

For a command-line preview, use `ffplay`:

```sh
ffplay -f avfoundation -framerate 5 -video_size 320x240 \
  -i "UVC CAM1:none"
```

The preview continues until the window closes or you press `q`. macOS can request camera access for Terminal.

## Flash again

UVC mode hides the serial port. Enter download mode:

1. Hold **BOOT**.
2. Press and release **RESET**.
3. Release **BOOT**.
4. Run the flash command again with the new `/dev/cu.usbmodem*` port.

## Data path

```text
XIAO camera → RGB565 frame → ESPDet-Pico inference → green boxes → MJPEG → USB UVC → macOS
```

The device uses no Wi-Fi and sends no image data outside the USB connection.

## Upstream components

- [Espressif USB Device UVC](https://components.espressif.com/components/espressif/usb_device_uvc)
- [Espressif Cat Detect](https://github.com/espressif/esp-dl/tree/master/models/cat_detect)
- [Espressif ESP32 Camera](https://components.espressif.com/components/espressif/esp32-camera)
- [Seeed XIAO ESP32S3 Sense camera pin map](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/)
