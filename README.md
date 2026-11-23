# 2H4U for the original Xbox (nxdk port)

**2H4U ("Too Hard For You")** is a mix of Tetris and Breakout by Pierre-Yves Ricau and Pierre Lagouge (2006). You drop tetrominoes at the top of the screen and keep a ball in play with a paddle at the bottom. Neobomb ported it to the original Xbox using the Microsoft XDK and the SDLx libraries.

This repository rebuilds that Xbox port with the open-source [nxdk](https://github.com/XboxDev/nxdk). It can be built and shared without the Microsoft XDK.

## What's different from the original Xbox release

- **Automatic resolution.** The game uses whatever video mode the console is set up for. That depends on the AV cable and the HDTV modes enabled in the dashboard. It tries 720p, then 1080i, then 800×600 over VGA, then 480p, then 480i/PAL. You no longer need to edit a text file.
- **Automatic scaling.** The game is drawn at 800×600, which is the size of all its artwork, and scaled to fill the screen at 4:3.
  - In HD modes the picture is enlarged (960×720 at 720p, 1440×1080 at 1080i).
  - In SD it is smoothly reduced to 640×480.
  - The old screen-adjust option (right-stick click) is gone.
- **Saves** go to `E:\UDATA\32483455\`. This is the same title ID as the original release.
- **Fixes:**
  - The language picker chose the opposite language to the one you picked.
  - Menu text showed a stray box character at the end of each line.
  - The game crashed when `data/RESOLUTION` was missing.

## Video mode override

`data/RESOLUTION` is optional. Leave it as `auto`, or put `480`, `720` or `1080` on its first uncommented line to prefer that mode. If the console can't output the mode you choose, the game falls back to `auto`.

## Controls

| Input | Action |
| --- | --- |
| Triggers / right stick | Move paddle |
| D-pad / left stick left, right | Move tetromino |
| D-pad / left stick down | Drop tetromino faster |
| D-pad up / B | Rotate tetromino |
| A | Fast drop / select |
| Start | Pause |
| Back | Back / exit |

## Building

You need the [nxdk prerequisites](https://github.com/XboxDev/nxdk/wiki/Install-the-Prerequisites): clang, lld, llvm, make, cmake, bison and flex.

```sh
git clone --recursive https://github.com/XboxDev/nxdk.git ../nxdk
make NXDK_DIR=../nxdk
```

This produces:

- `bin/`, which contains `default.xbe` and `data/`. Copy the whole folder to the Xbox (for example to `E:\Games\2H4U\`).
- `2H4U.iso`, a disc image for burning or for the xemu emulator.

GitHub Actions builds both on every push. Download them from the run's **Artifacts**.

### Desktop test build

`Makefile.host` builds the same code against desktop SDL2. It uses the Xbox controls and scaling, and is only meant for testing changes to the port without a console.

```sh
make -f Makefile.host
HOST_OUTPUT=1280x720 ./2h4u-host      # simulate an output resolution
```

## How the port works

| File | Role |
| --- | --- |
| `src/platform_xbox.cpp` | Picks the video mode with nxdk's `XVideoSetMode`, double-buffers the framebuffer, mounts `E:` for saves. |
| `src/scaler.cpp` | Scales the 800×600 frame to the screen: nearest-neighbour up, bilinear down. |
| `src/mixer.cpp` | Small replacement for the parts of SDL_mixer the game uses (nxdk has no SDL_mixer). Plays the WAV effects and streams the MP3 music, decoded with [dr_mp3](https://github.com/mackron/dr_libs). |
| `src/compat.h` | Maps the SDL 1.2 calls the game uses onto SDL2. |
| `src/Engine.cpp` | Main loop and controller input (SDL2 game controller API). |

`original/` holds the XDK project files from the original port, for reference.

## Credits and license

- Game by Pierre-Yves Ricau and Pierre Lagouge, GPL v2 or later (`COPYING.txt`, `README.txt`).
- Original Xbox port by Neobomb (`XBOX.README.txt`).
- CEngine framework by Anders "Sion" Nissen (GFDL 1.2, see `src/Engine.cpp`).
- dr_mp3 by David Reid, public domain / MIT-0.
- Fonts: see `data/DIGITAL.TXT`.

This repository is distributed under the GPL (see `LICENSE`).
