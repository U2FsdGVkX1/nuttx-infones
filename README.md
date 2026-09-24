# InfoNES on NuttX

A NES emulator for any NuttX board with a framebuffer. The [InfoNES](https://github.com/jay-kumogata/InfoNES) core (Apache-2.0) is an unmodified git submodule under `core/`; all NuttX integration is in `infones_main.cpp`.

## Installation

Clone into the NuttX apps tree as `games/infones`:

```sh
git clone --recursive git@github.com:U2FsdGVkX1/nuttx-infones.git apps/games/infones
```

Requirements:

- `CONFIG_HAVE_CXX=y` and `CONFIG_VIDEO_FB=y`, with an RGB555, RGB565, or RGB32 framebuffer.
- A mounted filesystem holding the `.nes` file.
- About 860 KiB of static RAM (mostly BSS), plus the ROM's PRG/CHR data.

## Configuration

| Option | Default | Description |
|---|---|---|
| `CONFIG_GAMES_INFONES` | `n` | Enable the application |
| `CONFIG_GAMES_INFONES_FBDEV` | `/dev/fb0` | Framebuffer device |
| `CONFIG_GAMES_INFONES_CONSOLE` | `y` | Use the console terminal as a controller |
| `CONFIG_GAMES_INFONES_KBDDEV` | `/dev/kbd0` | Event keyboard (requires `CONFIG_INPUT_KEYBOARD`) |
| `CONFIG_GAMES_INFONES_JOYDEV` | `/dev/djoy0` | Discrete joystick (requires `CONFIG_INPUT_DJOYSTICK`) |
| `CONFIG_GAMES_INFONES_AUDIODEV` | `/dev/audio/pcm0p` | Audio output (requires `CONFIG_AUDIO`) |

Input and audio devices are optional: an empty path disables one, and any device that is missing or fails at runtime is skipped.

## Usage

```sh
nsh> infones /path/to/game.nes
```

Only iNES 1.0 ROMs are supported, and battery-backed SRAM is not saved. The image is centred and scaled to fit the framebuffer.

| Input | Directions | A / B | Select / Start | Quit |
|---|---|---|---|---|
| Console terminal | W/S/A/D | K / J | U / I | Q |
| Event keyboard | arrows | Z / X | A / S | Q |
| Discrete joystick | directions | buttons 3 / 4 | buttons 1 / 2 | — |

The console needs a terminal on stdin. It sends no key releases, so each key press is held for a few frames.
