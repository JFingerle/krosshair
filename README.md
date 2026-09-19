# krosshair - Crosshair Overlay for Games on Linux

* Works on **Native / Non-Flatpak** systems (e.g. Arch Linux, CachyOS, Ubuntu etc.)
* Works on **Flatpak** systems (e.g.Bazzite).
* Works with **Steam** and **non-Steam** games.


## This Fork - Differences to `noahlyk/krosshair`

* Hotkey added to toggle the crosshair (press `SHIFT_R` + `F9`, see `Configuration` chapter below on how to change the hotkey)
* Rendering fixed on 4K and other resolutions ([PR](https://github.com/noahlyk/krosshair/pull/2))
* Memory leak fixed. VRAM usage is around 33 MiB (34 MB) on a 4K display (with a 4K transparent PNG crosshair, 37 KiB size)
* Flatpak build added ([PR](https://github.com/noahlyk/krosshair/pull/1))
* Code refactored and improved, unit tests added

<br>

# Demo - Default Dot Crosshair
Screenshot showing the default crosshair. To use a different crosshair place one of the files in repo dir `crosshairs` at `~/.config/crosshair-maker/projects/current.png` or use env var `KROSSHAIR_IMG` to load it from a different location.

<img src="img/quake-crosshair.png" alt="quake" width="800"/>

<br>

# Demo - Anti Motion Sickness Overlay
This is an overlay (not really a crosshair) which helps against motion sickness in first person games. You can find it in the repo: `crosshairs/anti-motionsickness-1_<resolution>.png`.

<img src="img/demo_anti-motionsickness-1.png" alt="quake" width="800"/>

<br>

# Installation

## Native / Non-Flatpak Installation

### Option 1: Download Release and Install manually

- Download the [latest `krosshair-linux-x86_64.tar.gz` release](https://github.com/JFingerle/krosshair/releases/latest)
- Run the following commands to copy the files to their destinations:
  ```
  sudo mkdir /usr/lib/krosshair/
  sudo cp krosshair.so /usr/lib/krosshair/krosshair.so
  sudo cp krosshair.json /usr/share/vulkan/implicit_layer.d/krosshair.json
  ```

### Option 2: Build and Install from Source

```bash
git clone https://github.com/jfingerle/krosshair.git
cd krosshair
make install
```

## Flatpak Installation

Necessary if your games (and apps like Steam / Lutris / Heroic) run via Flatpak. This is for example the case on Bazzite and other immutable Linux distributions.

After the installation you need to restart all Flatpak apps which you use to run games (Steam, Heroic, Lutris etc.).

### Option 1: Download Github Release
- Download **all** .flatpack files from the [releases page](https://github.com/jfingerle/krosshair/releases).
- Install via double-clicking the file in your file manager (or run `flatpak install (--user) filename.flatpak` in your terminal).


### Option 2: Build from Source

Builds and installs Flatpak bundles for the supported runtime versions (24.08, 25.08, 26.08).

```bash
sudo pacman -S flatpak-builder
git clone https://github.com/jfingerle/krosshair.git
cd krosshair
make flatpak-install
```

### Set Flatpak Permissions
If you want to use other crosshairs (instead of the default dot crosshair) you need to allow your flatpak apps to (read-only) access dir `~/.config/crosshair-maker/projects`. To set a new default crosshair pick a crosshair from the `crosshairs` dir of this repo and place it at `~/.config/crosshair-maker/projects/current.png`. You can also place multiple crosshairs in the directory and set the env var `KROSSHAIR_IMG` to pick one of them (e.g. `KROSSHAIR_IMG=~/.config/crosshair-maker/projects/plus.png %command%` for Steam games).

```
flatpak override --user --filesystem=~/.config/crosshair-maker/projects:ro
```

Afterwards you need to restart all Flatpak apps which you use to run games (Steam, Heroic, Lutris etc.).

<br>

# Configuration

Krosshair is configured via environment variables (see `Usage` below for how to set them for Steam and non-Steam games).

| Variable | Description |
|----------|-------------|
| `KROSSHAIR` | Set to `1` to enable the overlay. |
| `KROSSHAIR_IMG` | Path to the crosshair image to load (PNG, GIF or APNG). Defaults to `~/.config/crosshair-maker/projects/current.png`. |
| `KROSSHAIR_HOTKEY_TOGGLE` | Hotkey to toggle the crosshair, default `SHIFT_R+F9`. Any modifier (`shift_l`, `shift_r`, `ctrl_l`, `ctrl_r`, `alt_l`, `alt_r`) can be combined with a letter, digit, `f1`–`f24`, `space`, `tab`, `escape` etc. Example: `KROSSHAIR_HOTKEY_TOGGLE=ctrl_r+1`. |

<br>

# Usage

## Steam Games

Add the following launch option:

```
KROSSHAIR=1 %command%
```

To use a custom crosshair (for example from the `crosshairs` dir of this repo):

```
KROSSHAIR=1 KROSSHAIR_IMG=/optional/path/to/crosshair.png %command%
```

To use a different hotkey to toggle the crosshair add the `KROSSHAIR_HOTKEY_TOGGLE` env var like this:

```
KROSSHAIR=1 KROSSHAIR_HOTKEY_TOGGLE=SHIFT_R+F7 %command%
```

## Non-Steam Games

```bash
export KROSSHAIR=1 # to enable Krosshair
export KROSSHAIR_IMG=/path/to/crosshair.png # To use a custom crosshair (for example from the `crosshairs` dir of this repo)
export KROSSHAIR_HOTKEY_TOGGLE=SHIFT_R+F7 # To use a different hotkey to toggle the crosshair
your-game
```

<br>

# Make your own crosshairs using `crosshair-maker`

krosshair works out of the box with [crosshair-maker](https://github.com/noahlyk/crosshair-maker), a crosshair overlay creator with SVG rendering and preview. The currently selected crosshair is automatically exported to `~/.config/crosshair-maker/projects/current.png`, which krosshair picks up as its default — just launch your game with `KROSSHAIR=1` and go.

Use `export KROSSHAIR_IMG=/path/to/crosshair.png` to use a specific crosshair file.

To test the full setup together with `vkcube`:

```sh
$ yay -Sy krosshair crosshair-maker vulkan-tools
$ KROSSHAIR=1 vkcube &
$ crosshair-maker &
```
<br>

# FAQ / Various

## Can i get banned for this?
This project is quite similar to `MangoHud` so it should be safe to use, but use it at your own risk.

## Why does Krosshair not work?
- Flatpak: Restart your Flatpak apps like Steam/Lutris/Heroic which you use to start your games. Or simply reboot after the installation.
- Check the logs, see section `Are there any logs which I can use to troubleshoot issues?` below?

## What hotkeys can I use?
The hotkey to toggle the crosshair can be set via env var `KROSSHAIR_HOTKEY_TOGGLE`, see `Configuration` for the allowed syntax.

## Why does it not use my crosshair file?
The layer prints diagnostic messages to `stderr`. Look for lines starting with `[KH]`:

- `[KH] Loading crosshair from file '<path>'. Reason: ...` — the crosshair loaded successfully from that file.
- `[KH] Cannot load crosshair image '<path>' (...): <cause> — falling back to the built-in crosshair` — the file failed to load. The `<cause>` says why, e.g. `No such file or directory`, `Permission denied`, or `not a valid image file`. The layer then falls back to the built-in dot crosshair.
- `[KH] Using built-in crosshair. Load a different crosshair by setting env var 'KROSSHAIR_IMG'...` — no crosshair file was found at the default location (`~/.config/crosshair-maker/projects/current.png`) and `KROSSHAIR_IMG` is not set.

Common causes:
- **File does not exist** — check the path is correct. If using `KROSSHAIR_IMG`, make sure the file exists at the given path.
- **Not a valid image** — the file must be a PNG, GIF, or APNG.
- **Flatpak permissions** — Flatpak apps cannot read arbitrary files. You need to grant access to the crosshair file/dir, e.g.:
  ```
  flatpak override --user --filesystem=/path/to/crosshair.png:ro
  ```
  Afterwards restart all Flatpak game launchers (Steam, Heroic, Lutris etc.).

## Are there any logs which I can use to troubleshoot issues?
Krosshair prints `[KH]` log messages to `stderr`.
- **Native games** — The messages appear directly in the games terminal output.
- **Steam (Proton) games** — set `PROTON_LOG=1` in a launch option of a game to redirect it's output to a log file. Example launch option:
  ```
  PROTON_LOG=1 KROSSHAIR=1 %command%
  ```
  The log file is written to `~` (native Steam installation) or `~/.var/app/com.valvesoftware.Steam` (Flatpak installation).
- **Performance Debugging** — Set env var `KROSSHAIR_PERFLOGGING=1` to see performance related log messages like memory usage.