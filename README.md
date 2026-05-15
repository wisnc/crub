# crub
---

A lightweight bootloader/firmware flasher for M5Cardputer ADV with disk partition management and a UNIX-like file manager.

---

boot screen

<img src="crub2.jpg" width="300">

partition info

<img src="crub6.jpg" width="300">

running `flash binary.bin`

<img src="crub5.jpg" width="300">

editor

<img src="crub7.jpg" width="300">

overall

<img src="crub1.jpg" width="300">

---

## Building

```
git clone https://github.com/wisnc/crub
cd crub
pio run -e bootloader
pio run -e m5cardputer
```

## Installing

Since this is a merged firmware, you need to use `esptool`.

```bash
python -m esptool --chip esp32s3 --port <COM PORT> --baud 1500000 --before default_reset --after hard_reset write_flash -z 0x0 .pio\build\bootloader\bootloader.bin 0x8000 .pio\build\m5cardputer\partitions.bin 0x10000 .pio\build\m5cardputer\firmware.bin
```

make sure to replace `<COM PORT>` with your cardputer serial port.

## How to use

commands can be viewed with `help`

display can also be controlled with Fn + _ and Fn + = for dimmer and brighter display. also Btn0 toggles display to save battery

---

## Version History / Changelog

### v1.8

- nvs gets overwritten every time a firmware is being flashed. this was on 1.7 when i added functionality to name what firmware is inside ota_0 by writing it in nvs. it is also where the aliases are saved. fixed it by moving those to SD (firmware name and alias). because who would use this firmware flasher and file manager without an SD right? 

### v1.7

- Public release
- GitHub repository created
