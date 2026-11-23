First release of **2H4UX** rebuilt with the open-source [nxdk](https://github.com/XboxDev/nxdk), from Neobomb's original Xbox port of 2H4U. Tested on real hardware.

### Downloads
- **2H4UX-xbox.zip**: copy the `2H4UX` folder to your Xbox (for example `E:\Games\2H4UX\`).
- **2H4UX.iso**: a disc image.

### What's new compared with the original 2H4UX
- **Automatic resolution.** The game uses whatever your console is set up for: 720p, 1080i, 480p, 480i/PAL or VGA, depending on your AV cable and the HD modes enabled in the dashboard. `data/RESOLUTION` can still force 480, 720 or 1080.
- **Automatic scaling.** The picture fills the screen at 4:3, so the old screen-adjust option is gone.
- **Clean quit.** Quitting shows a "Quitting!" screen and returns to the dashboard.
- **Fixes:**
  - The language picker chose the opposite language to the one you picked.
  - Stray box characters appeared at the end of menu text.
  - The game crashed when `data/RESOLUTION` was missing.
- **Saves** stay in `E:\UDATA\32483455\`. This is the same title ID as the original, so existing saves and high scores carry over.

### Known issues
- The xemu emulator is not supported. Depending on the BIOS, the picture can strobe or show the wrong buffer, and there is no sound.
