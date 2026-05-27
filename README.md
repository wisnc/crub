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

fetch

<img src="crub8.jpg" width="300">

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

flash binaries from SD with
```
flash /path/to/binary.bin
```

then launch it with

```
launch
```

aliases can be externally edited (or with the built in editor) and is located as a text file in root directory in

```
/.crub_aliases
```


create aliases for flashing and launch: e.g. shortcut flash and launch bruce as "br"

```
alias br "bruce /binaries/bruce.bin && launch"
```

view binary partition scheme

```
bininfo /path/to/binary.bin
```

run scripts from SD

```
run /path/to/script.sh
```

scripts are just text files with one command per line. lines starting with `#` are comments. you can use `echo`, `sleep`, and any other crub command inside them

boot scripts run automatically if `/.crub_boot` exists on the SD card

you can also quickly do calculations. start the expression with '='. for example

```
>=5*3
15
>=2.5*(3+1)
10
>=100/3
33.3333
>=17%5
2
>=(2+3)*(7-1)
30
```

commands can be viewed with `help`

display can also be controlled with Fn + _ and Fn + = for dimmer and brighter display. also Btn0 toggles display to save battery

### editor

`edit (filename)` to enter nano-like editor

`Fn + , . / ;` for left down right up

`Fn + backspace` for exit/save. moves to status line and prompts directory to save and filename. enter to save Fn + backspace to discard
  
`Fn + C` copy line

`Fn + V` paste line

`Fn + X` cut line

`Opt + .` move to end of the file

`Opt + ;` move to top of the file

### USB

run command `usbsd` to enter usb mode. plug in to PC to mount SD from the cardputer itself

---

## Version History / Changelog

### v2.6.5

- added `usbsd`

### v2.6

- added file utilities: `head`, `find`, `tree`, `wc`, `hex`
- added system commands: `uptime`, `free`, `i2cscan`, `md5`, `beep`
- added scripting: `run`, `echo`, `sleep`, `#` comments
- added `/.crub_boot` boot script. runs on every boot if it exists on SD
- shrunk test partition from 1536K to 704K. ota_0 is now 5824K
- partinfo now shows unallocated flash space
- updated help to show all commands

### v2.5

- added basic calculator feature


### v2.4

- fixed an issue: aliases only having 16 entries as limit. now it is practically infinite

### v2.3b

- added Cardputer v1.1 support. thanks u/First-Preference5831

### v2.3

- added fetch

### v2.2

- fixed an issue where larger binaries are unflashable. new functionality to view binary partition scheme `bininfo /path/to/binary.bin`

### v2.1

- fixed an issue where having a LoRa Cap (or any Cap on that matter) interferes with reading the SD on boot

### v2.0

- updated UI
- added a nano-like text editor
- probably final update as no other features are planned

### v1.8

- nvs gets overwritten every time a firmware is being flashed. this was on 1.7 when i added functionality to name what firmware is inside ota_0 by writing it in nvs. it is also where the aliases are saved. fixed it by moving those to SD (firmware name and alias). because who would use this firmware flasher and file manager without an SD right? 

### v1.7

- Public release
- GitHub repository created
