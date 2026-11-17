<p align="center"><img src="res/topbar.ico" width="72" alt="TopBar icon"></p>

<h1 align="center">TopBar</h1>

<p align="center"><b>A Waybar-style status bar for Windows 10 and 11 that stays out of your way.</b><br>
Open apps on the left, the clock in the middle, your whole system at a glance on the right.<br>
One 400 KB native exe. No installer, no admin rights, no drivers, no background services.</p>

![TopBar across a 1440p screen](docs/bar.png)
![The right side of the bar: network speed, Ethernet, CPU with per-core bars and temperature, GPU, memory, disk, volume, Bluetooth, battery, power](docs/bar-right.png)

## What it shows

| Module | What you see | Click | Other |
|---|---|---|---|
| **Windows** | Icons of your open apps, the active one with its title | Switch to / minimize | Middle-click closes, right-click for a window menu |
| **Clock** | Date and time in your Windows format | Calendar & notifications | Hover for week number, day of year, uptime |
| **Net speed** | Live download / upload | Network flyout | Hover for totals since boot |
| **Network** | Ethernet, Wi-Fi name and signal, or Offline | Network flyout | Hover for adapter, IP, link speed, internet status |
| **CPU** | Usage, tiny per-core bars, temperature | Task Manager | Hover for clock speed, a 1-minute graph and a live bar for every core and thread |
| **GPU** | Usage and temperature (NVIDIA, AMD and Intel) | Task Manager | Hover for VRAM and every other GPU |
| **Memory** | RAM usage % and GB | Task Manager | Hover for available and committed memory |
| **Disk** | System drive usage | This PC | Hover for every drive |
| **Volume** | Level / muted | Mute | Scroll to change, right-click for the mixer |
| **Bluetooth** | On/off, connected device | Bluetooth settings | Hover for all connected devices |
| **Battery** | Charge and charging state (laptops only) | Power settings | Hover for time left |
| **Power** | | Animated power menu | Right-click for a quick list |

Modules that don't apply to your PC (no battery, no Bluetooth, no Wi-Fi) hide themselves.

### Hover for the details

Every module has a live tooltip that keeps updating while it's open. The CPU tooltip shows each
physical core with one bar per hardware thread.

![Tooltips for CPU, GPU, network, memory, clock and network traffic](docs/tooltips.png)

## Power menu

Click the power icon and the screen dims while big round buttons spring in one after another.
Hover one and it grows and glows: the restart arrow spins, the moon rocks, the power icon pulses.

![The animated power menu](docs/power-menu.gif)

- **Lock, Sleep, Hibernate** and **Display off** (screen off while the PC keeps running, so downloads continue) are one click.
- **Sign out, Restart and Shut down need a press-and-hold** while a ring fills, so a stray click can
  never turn your PC off.
- Arrow keys and Enter work too. Esc, right-click or a click outside closes it.

## Games and movies

TopBar reserves space at the top of each monitor, like the taskbar does, so maximized windows sit
neatly below it. When something goes **fullscreen** on a monitor (a game, a video, a slideshow):

- The bar on that monitor **hides**, and if no bar is visible, TopBar **stops collecting stats entirely**:
  zero work while you play.
- **Watching a video?** Rest the mouse on the very top edge for a moment to peek at the bar, macOS-style.
  It slides away again when you move the mouse off.
- **Playing a game?** Games capture or hide the mouse, so the bar never pops up by accident.
  Press **Ctrl + Alt + B** to peek at it for a few seconds.
- With multiple monitors, only the monitor running fullscreen loses its bar. The other keeps
  showing your CPU/GPU temps while you play.

Outside fullscreen, **Ctrl + Alt + B** hides the bar and gives the screen space back. Press it
again to bring the bar back.

## Light on your PC

- Native C++ with no runtime or framework. About 12–16 MB of RAM.
- About **0.04% of total CPU** on an 8-core laptop while visible, and 0% while hidden.
- Draws in software (Direct2D), so it never touches the GPU your games are using.
- Runs in Windows *efficiency mode* at below-normal priority. Refreshes every second, or every 2 s on battery.
- Expensive readings only run while you're looking at them. For example, CPU clock speed is only read
  while the CPU tooltip is open, and GPU engines that sit idle are checked every few seconds instead of every second.
- Pauses completely when the screen is off or the PC is locked.
- Uses no internet. The network speed is read from your adapters, not from a speed test.

## Works on any PC?

Yes. TopBar is built to adapt rather than assume:

| | |
|---|---|
| **Windows** | Windows 10 and 11, 64-bit. The exe only imports functions that exist since Windows 8.1, and newer features are looked up at runtime. On Windows 10 it falls back to the Windows 10 fonts and icons. |
| **CPU** | Any Intel or AMD CPU. Per-core bars cover up to 64 threads. |
| **GPU** | Any GPU with a modern (WDDM 2) driver: NVIDIA, AMD, Intel, discrete or integrated. Laptops with two GPUs show the dedicated one, and hovering shows both. If the driver doesn't report a temperature, the temperature simply isn't shown. |
| **Sensors** | CPU temperature uses the thermal zone Windows exposes without admin rights. Most laptops have it, and where it's missing the reading hides. |
| **Hardware** | No battery, Bluetooth or Wi-Fi? Those modules hide themselves. Plugging in a dock, a monitor or a Bluetooth adapter is picked up automatically. |
| **Monitors** | One bar per monitor, each sized for that monitor's scaling (100%, 125%, 150%, …). Unplugging or adding a screen rebuilds the bars. |
| **Windows Update** | Uses only documented Windows APIs, with no hooks into Explorer and no injected code. If Explorer restarts (for example after an update), the bar re-registers itself. Kernel GPU statistics are read into padded buffers, so a future Windows build with bigger structures can't corrupt memory. A sensor that stops working just hides. |
| **ARM PCs** | Windows 11 on ARM runs the x64 build through its built-in emulation. |

## Install

1. Download `TopBar.exe` from the [releases page](../../releases) (or build it, see below).
2. Put it in a folder of your choice (for example `%LOCALAPPDATA%\TopBar`) and run it.
3. Right-click an empty part of the bar and tick **Start with Windows**.

To remove it, untick **Start with Windows**, choose **Exit TopBar**, and delete the folder.

## Settings

Right-click the bar and pick **Edit settings…**. This opens `topbar.ini` (created next to the exe,
or in `%APPDATA%\TopBar` if that folder is read-only). Changes apply as soon as you save.

```ini
[bar]
monitors = all                 ; or: primary
height = 30
modules_left = windows
modules_center = clock
modules_right = netspeed, network, cpu, gpu, memory, disk, volume, bluetooth, battery, power
gpu = auto                     ; auto (the dedicated GPU), all, or an index
cpu_usage = time               ; or: taskmanager
interval_ms = 1000

[behavior]
hide_in_fullscreen = 1
reveal_on_hover = 1
peek_hotkey = Ctrl+Alt+B
game_apps =                    ; exe names that should always hide the bar, e.g. game.exe

[colors]
background = #11111b           ; Catppuccin Mocha by default; any #rrggbb works
```

Every option is documented inside the file.

### Command line

```
TopBar.exe --toggle    hide / show the running bar
TopBar.exe --reload    reload settings
TopBar.exe --exit      quit
TopBar.exe --debug     write a timing log to %TEMP%\topbar.log
```

## Notes

- **Why the CPU number differs from Task Manager.** TopBar shows *busy time*: the share of each
  second the cores were actually working. It's a direct measurement with a fixed meaning (0% = idle,
  100% = no idle time left), and it's what Linux, macOS, Process Explorer and Resource Monitor report.
  Since Windows 8, Task Manager instead shows *utility*: busy time multiplied by the current clock speed
  relative to the CPU's base clock. That reads lower when the CPU idles at low clocks and higher when
  it boosts, and it can show 100% while there is still headroom. TopBar shows clock speed as its own
  number and lists Task Manager's reading in the CPU tooltip, so you see both facts separately. Set
  `cpu_usage = taskmanager` if you prefer Task Manager's method. Two tools that sample at different
  moments will always drift by a few percent.
- **CPU temperature** comes from the ACPI thermal zone that Windows exposes without admin rights.
  On most laptops it tracks the CPU package. On some desktops it isn't available, and then it is hidden.
- **Wi-Fi name** needs *Location → Let desktop apps access your location* on Windows 11 24H2 and later.
  Without it, TopBar shows "Wi-Fi".
- **Hybrid-graphics laptops:** while the dedicated GPU is powered down, TopBar doesn't query its
  temperature, so it never keeps the GPU awake.

## Build from source

Requires Visual Studio 2019 or newer with the **Desktop development with C++** workload.

```
build.cmd          :: release build -> build\TopBar.exe
build.cmd debug    :: debug build with symbols
```

Every push is also built by GitHub Actions, and a `v*` tag publishes a release with the exe attached.

Source layout:

| File | Purpose |
|---|---|
| `src/app.cpp` | Controller: monitors, timers, fullscreen detection, peek, menus, tooltip text |
| `src/bar.cpp` | One bar per monitor: appbar, layout, drawing, mouse; tooltip popup |
| `src/stats.cpp` | CPU (busy time, per-core, clock speed), memory, network, GPU (D3DKMT), temperature, disks, battery, Wi-Fi, Bluetooth |
| `src/power.cpp` | Animated power menu (spring animations, press-and-hold confirm) |
| `src/audio.cpp` | Default output volume (event driven) |
| `src/winlist.cpp` | Open windows and their icons (including Store apps) |
| `src/render.cpp` | Software Direct2D / DirectWrite renderer |
| `src/config.cpp` | `topbar.ini` loading, defaults and live reload |
