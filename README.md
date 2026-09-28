# UltraHLE-AI

A personal project: the classic UltraHLE Nintendo 64 emulator, ported to 64-bit Windows and extended well beyond the original release.

**This is not meant to replace any other emulator.** It was built for AI training and experimentation. If you want to play N64 games, use a mature, maintained emulator such as ares, Mupen64Plus or Project64.

No ROMs, BIOS files or other copyrighted game data are included. You need your own legally obtained ROM images.

## What's new

UltraHLE's original high-level emulation (HLE) core is still there, but most of the machine has been rebuilt around it:

- **64-bit build.** Ported from 32-bit x86 (with inline assembler) to x64, built with Visual Studio 2022.
- **OpenGL 3.3 renderer.** Replaces the Glide 2 dependency. No Glide wrapper is needed.
- **Full RSP emulation.** A real Reality Signal Processor (vector unit included, based on cxd4's RSP) runs graphics and audio microcode directly. HLE graphics and audio are still used where they work.
- **New LLE engine.** A low-level emulation mode that boots games through the real N64 OS instead of patching it. It covers PI/SI/VI/AI timing, interrupts, the AI FIFO and the RDP command path. `osmode=` in `ULTRA.INI` picks HLE or LLE per game.
- **libdragon support.** Homebrew built with libdragon runs on the real RSP, sending raw RDP commands to the renderer.
- **Passes the n64-systemtest test ROM.** The only remaining failures are four RDP pixel-accuracy tests that are out of scope for a hardware-accelerated renderer.
- **Xbox controller support** through XInput, with keyboard fallback.
- **N64 colour combiner as shaders**, render-to-texture, S2DEX backgrounds and sprites, and many microcode fixes (F3DEX/F3DEX2, Conker, Rogue Squadron and others).
- **Saves and accessories.** EEPROM, SRAM, FlashRAM, Controller Pak and Rumble Pak, plus save states.
- **Expansion Pak (8 MB)** for games configured with `rdram=8`.
- **Zipped ROMs** can be loaded directly.

## Building

Requirements:

- Windows 10 or 11, 64-bit
- Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143 toolset and the Windows 10/11 SDK)

Steps:

1. Open `Scripts\UltraHLE.sln` in Visual Studio 2022.
2. Select the **Release | x64** configuration. **Debug | x64** also works and builds `UltraHLE64d.exe`.
3. Build the solution.

The executable is written to `Build\UltraHLE64.exe`.

From a Developer Command Prompt you can also run:

```
msbuild Scripts\UltraHLE.sln /p:Configuration=Release /p:Platform=x64
```

The Win32 configurations are left over from the original project and are no longer maintained.

## Running

Keep `UltraHLE64.exe` and `ULTRA.INI` together in the `Build` folder. `ULTRA.INI` holds the per-game settings (OS mode, RAM size, controller accessory, timing). The `saves`, `states` and `snap` folders are created next to the executable when they are first needed.

<!-- TODO: usage, controls, screenshots -->

## Layout

- `src` - emulator source (C, plus one C++ file)
  - `src/rsp_cxd4` - RSP interpreter
  - `src/xgl` - OpenGL renderer
  - `src/zip` - miniz, used for zipped ROMs and PNG snapshots
- `Scripts` - Visual Studio 2022 solution and project
- `Build` - prebuilt executable and `ULTRA.INI`

## Credits

- UltraHLE by Epsilon and RealityMan (1999). Sources from the original release: https://code.google.com/archive/p/ultrahle/downloads
- RSP interpreter based on cxd4's RSP plugin
- miniz by Rich Geldreich (public domain / Unlicense)
- ares, Mupen64Plus, GLideN64 and the N64 decompilation projects were used as hardware and behaviour references

## License

See [LICENSE](LICENSE).
