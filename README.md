# 3Beans
A low-level 3DS emulator.

### Overview
3Beans emulates the 3DS at a low level, which means that it runs the entire OS as if it were on real hardware. It can
boot the home menu and launch some games, but it's still young and has plenty of issues. It has both software and
hardware GPU rendering, but CPUs are still fully interpreted. My goal for this project is to achieve viable speeds with
full low-level emulation, and maybe explore some high-level elements down the road.

### This fork
This repository is a fork of [Hydr8gon/3Beans](https://github.com/Hydr8gon/3Beans) that adds what's needed to
run real firmware images on the emulated hardware (Linux ports such as Cyano3DS), and to drive the emulator from
scripts. On top of upstream it has:

* MMU aborts, GPIO3, and SD CSD handling for ARM9/ARM11 firmware boots
* SMMLx instructions (CP14 SLEEP/WAKE/STANDBY), PDN sleep/wake with a closable lid shell
* MCU and CSND accuracy fixes, plus GPU command tracing
* A debug interface: TCP control socket, GDB stub, and a raw guest serial console
* A headless frontend (`3beans-headless`) that runs without a window or audio

Upstream remains the reference implementation; bugs in unmodified upstream code are worth reporting there.

### Downloads
3Beans is available for Windows, macOS, and Linux. The latest builds are automatically provided via GitHub Actions,
and can be downloaded from the [releases page](https://github.com/Hydr8gon/3Beans/releases).

### Setup
To function, 3Beans requires files dumped from a physical 3DS with homebrew access. Old 3DS is preferable for the best
experience; new 3DS has more issues and is much slower. At minimum, you'll need `boot9.bin`, `boot11.bin`, and
`nand.bin`, all of which can be obtained using [GodMode9](https://github.com/d0k3/GodMode9). You might also want to
create `sd.img`, which can be any [FAT-formatted image file](https://kuribo64.net/get.php?id=mRJJ5GggXOPbKUMZ) to serve
as an SD card. These files can be configured in the path settings.

### Usage
Once the necessary files are present, 3Beans should function like the system they were dumped from. Controls can be
viewed and changed in the Settings menu. You can boot without a cartridge through the System menu, or select one through
the File menu. If you want to skip the home menu and load directly into a game, you can use the Cart Auto-Boot setting.
Note that 3Beans requires encrypted cartridge dumps, unlike high-level emulators which typically expect decrypted ones.
GodMode9 can dump both encrypted and decrypted ROMs, so make sure you get the right one.

### Contributing
This is a personal project, and I've decided to not review or accept pull requests for it. If you want to help, you can
test things and report issues or provide feedback. If you can afford it, you can also donate to motivate me and allow me
to spend more time on things like this. Nothing is mandatory, and I appreciate any interest in my projects, even if
you're just a user!

### Building
**Windows:** Install [MSYS2](https://www.msys2.org) and run the command
`pacman -Syu mingw-w64-ucrt-x86_64-{gcc,pkg-config,wxwidgets3.3-msw,portaudio,libepoxy,jbigkit} make` to get
dependencies. Navigate to the project root directory and run `make -j$(nproc)` to start building.

**macOS/Linux:** On the target system, install [wxWidgets](https://www.wxwidgets.org) (v3.3.2 or higher),
[PortAudio](https://www.portaudio.com), and [Epoxy](https://github.com/anholt/libepoxy). This can be done with a package
manager like [Homebrew](https://brew.sh) on macOS, or a built-in one on Linux. Run `make` in the project root directory
to start building.

### Headless frontend
`make headless` builds `3beans-headless`, which runs the core without a window or audio and is controlled over TCP.
The settings folder and file names are the same as the GUI's (`3beans.ini`); command line options override them.

```
3beans-headless [--dir SETTINGSDIR] [--port N] [--frames N] [--no-limit] [--log N]
                [--firm FILE] [--sd FILE] [--system-type 0|1|2]
```

Three servers are opened on `127.0.0.1`, on the port given by `--port` (default 4900) and the following two:

| port | protocol | purpose |
| --- | --- | --- |
| N | line-based control | debug commands, one per line; replies are plain text |
| N+1 | GDB remote serial protocol | attach a GDB with an ARM target |
| N+2 | raw bytes | guest console UART (`0x1017F000`) in and out |

Send `help` to the control port for the full command list. Highlights:

```
status | regs [cpu] | pause | resume | step [cpu]      (cpu: 0-3 ARM11, 9 ARM9)
bp add|del ADDR [cpumask] | watch add r|w|a ADDR [len] | watch list
mem v|p CPU ADDR LEN | memw v|p CPU ADDR HEXBYTES | faults [N]
shot FILE.png [top|bottom|both] | key NAME [ms] | touch X Y [ms]
lid open|close | conwrite TEXT | conread | conlog [N]
log level N | trace CPU LO HI LO2 HI2 FILE | quit
```

A minimal control client in Python:

```python
import socket
s = socket.create_connection(("127.0.0.1", 4900))
s.sendall(b"status\n")
print(s.recv(4096).decode())
```

### References
* [GBATEK](https://problemkaputt.de/gbatek.htm) - Incomplete but great reference for the 3DS hardware
* [3DBrew](https://www.3dbrew.org) - Comprehensive wiki covering high- and low-level details
* [libctru](https://github.com/devkitPro/libctru) - A homebrew library that shows how to interact with the OS
* [Teakra](https://github.com/wwylele/teakra) - The only source for newer aspects of the Teak architecture
* [Corgi3DS](https://github.com/PSI-Rockin/Corgi3DS) - The first LLE 3DS emulator, whose logs helped me debug

### Other Links
* [Hydra's Lair](https://hydr8gon.github.io) - Blog where I may or may not write about things
* [Discord Server](https://discord.gg/JbNz7y4) - A place to chat about my projects and stuff
