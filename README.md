# ORCΛ

Orca is an [esoteric programming language](https://en.wikipedia.org/wiki/Esoteric_programming_language) and live editor designed to quickly create procedural sequencers. Every letter of the alphabet is an operation, lowercase letters execute on `*bang*`, and uppercase letters execute each frame.

This is the C implementation of the [ORCΛ](https://wiki.xxiivv.com/site/orca.html) language and terminal livecoding environment. It's designed to be power efficient. It can handle large files, even if your terminal is small.

Orca is not a synthesizer, but a flexible livecoding environment capable of sending MIDI, OSC, and UDP to your audio/visual interfaces like Ableton, Renoise, VCV Rack, or SuperCollider.

<img src='https://raw.githubusercontent.com/wiki/hundredrabbits/Orca-c/PREVIEW.jpg' width='600'/>

| Main git repo | GitHub mirror |
| ------------- | ------------- |
| [git.sr.ht/~rabbits/orca](https://git.sr.ht/~rabbits/orca) | [github.com/hundredrabbits/Orca-c](https://github.com/hundredrabbits/Orca-c) |

## Quick Start for Debian/Raspbian (Raspberry Pi)

```sh
sudo apt-get install git libncurses5-dev libncursesw5-dev libportmidi-dev
git clone https://github.com/hundredrabbits/Orca-c.git
cd Orca-c
make          # Compile orca
build/orca    # Run orca
```

To choose your MIDI output device, press `F1` (or `Ctrl+D`) to open the main menu, and then select `MIDI Output...`

```
┌ ORCA ───────────────┐┌ PortMidi Device Selection ─────┐
│   New               ││ > (*) #0 - Midi Through Port-0 │
│   Open...           ││   ( ) #2 - ES1371              │
│   Save              │└────────────────────────────────┘
│   Save As...        │
│                     │
│   Set BPM...        │
│   Set Grid Size...  │
│   Auto-fit Grid     │
│                     │
│   OSC Output...     │
│ > MIDI Output...    │
│                     │
│   Clock & Timing... │
│.....................│
```

## Prerequisites

Core library: A C99 compiler (no VLAs required), plus enough libc for `malloc`, `realloc`, `free`, `memcpy`, `memset`, and `memmove`. (Also, `#pragma once` must be supported.)

Command-line interpreter: The above, plus POSIX, and enough libc for the common string operations (`strlen`, `strcmp`, etc.)

Livecoding terminal UI: The above, plus ncurses (or compatible curses library), and floating point support (for timing.) Optionally, PortMidi can be used to enable direct MIDI output.

## Build

The build script, called simply `tool`, is written in POSIX `sh`. It should work with `gcc` (including the `musl-gcc` wrapper), `tcc`, and `clang`, and will automatically detect your compiler. You can manually specify a compiler with the `-c` option.

Currently known to build on macOS (`gcc`, `clang`, `tcc`) and Linux (`gcc`, `musl-gcc`, `tcc`, and `clang`, optionally with `LLD`), and Windows via cygwin or WSL (`gcc` or `clang`, `tcc` untested).

There is a fire-and-forget `make` wrapper around the build script.

PortMidi is an optional dependency. It can be enabled by adding the option `--portmidi` when running the `tool` build script.

Mouse awareness can be disabled by adding the `--no-mouse` option.

### Build using the `tool` build script

Run `./tool help` to see usage info. Examples:

```sh
./tool build -c clang-7 --portmidi orca
    # Build the livecoding environment with a compiler
    # named clang-7, with optimizations enabled, and
    # with PortMidi enabled for MIDI output.
    # Binary placed at build/orca

./tool build -d orca
    # Debug build of the livecoding environment.
    # Binary placed at build/debug/orca

./tool build -d cli
    # Debug build of the headless CLI interpreter.
    # Binary placed at build/debug/cli

./tool clean
    # Same as make clean. Removes build/
```

### Build using the `make` wrapper

```sh
make release    # optimized build, binary placed at build/orca
make debug      # debugging build, binary placed at build/debug/orca
make clean      # removes build/
```

The `make` wrapper will enable `--portmidi` by default. If you run the `tool` build script on its own, `--portmidi` is not enabled by default.

## `orca` Livecoding Environment Usage

```
Usage: orca [options] [file]

General options:
    --undo-limit <number>  Set the maximum number of undo steps.
                           If you plan to work with large files,
                           set this to a low number.
                           Default: 100
    --initial-size <nxn>   When creating a new grid file, use these
                           starting dimensions.
    --bpm <number>         Set the tempo (beats per minute).
                           Default: 120
    --seed <number>        Set the seed for the random function.
                           Default: 1
    --rate <ratio>         Set the local tempo multiplier, as a fraction
                           or a whole number. Examples: 2, 3, 1/2, 1/4,
                           3/2. Default: 1
    --sync <addr:port>     Join an instance sync group at this address and
                           port, e.g. 127.0.0.1:9999. The first instance
                           to start is the master; later ones follow its
                           tempo and start/stop.
    --sync-multicast <group:port>
                           Like --sync, but join a multicast group so
                           instances on the same machine or LAN find each
                           other automatically. Example: 239.255.0.1:9999
    -h or --help           Print this message and exit.

OSC/MIDI options:
    --strict-timing
        Reduce the timing jitter of outgoing MIDI and OSC messages.
        Uses more CPU time.

    --osc-midi-bidule <path>
        Set MIDI to be sent via OSC formatted for Plogue Bidule.
        The path argument is the path of the Plogue OSC MIDI device.
        Example: /OSC_MIDI_0/MIDI
```

### Example: build and run `orca` livecoding environment with MIDI output

```sh
$ ./tool build --portmidi orca           # compile orca using build script
$ build/orca                             # run orca
```

### `orca` Livecoding Environment Controls

```
┌ Controls ───────────────────────────────────────────┐
│           Ctrl+Q  Quit                              │
│       Arrow Keys  Move Cursor                       │
│     Ctrl+D or F1  Open Main Menu                    │
│   0-9, A-Z, a-z,  Insert Character                  │
│    ! : % / = # *                                    │
│         Spacebar  Play/Pause                        │
│ Ctrl+Z or Ctrl+U  Undo                              │
│           Ctrl+X  Cut                               │
│           Ctrl+C  Copy                              │
│           Ctrl+V  Paste                             │
│           Ctrl+S  Save                              │
│           Ctrl+F  Frame Step Forward                │
│           Ctrl+R  Reset Frame Number                │
│ Ctrl+I or Insert  Append/Overwrite Mode             │
│        ' (quote)  Rectangle Selection Mode          │
│ Shift+Arrow Keys  Adjust Rectangle Selection        │
│   Alt+Arrow Keys  Slide Selection                   │
│   ` (grave) or ~  Slide Selection Mode              │
│           Escape  Return to Normal Mode or Deselect │
│  ( ) _ + [ ] { }  Adjust Grid Size and Rulers       │
│          < and >  Adjust BPM                        │
│          F2..F7  Tempo Multiplier (1/4..4x)         │
│                ?  Controls (this message)           │
└─────────────────────────────────────────────────────┘
```

## `cli` command-line interface interpreter

The CLI (`cli` binary) reads from a file and runs the orca simulation for 1 timestep (default) or a specified number (`-t` option) and writes the resulting state of the grid to stdout.

```sh
cli [-t timesteps] infile
```

You can also make `cli` read from stdin:
```sh
echo -e "...\na34\n..." | cli /dev/stdin
```

## Running several instances together

Two or more `orca` instances can share one tempo and transport, so starting
and stopping happens everywhere at once while each grid runs at its own
multiple of the beat.

```sh
build/orca --sync 127.0.0.1:9999 --bpm 120     # master
build/orca --sync 127.0.0.1:9999 --rate 2      # follower, double speed
build/orca --sync 127.0.0.1:9999 --rate 1/2    # follower, half speed
```

The first instance to start becomes the master and the rest follow it. A new
follower holds its first frames until the master reaches a bar boundary, then
adopts the master's frame counter, so it comes in on the downbeat instead of
whenever its boot happened to finish. After that it adopts the master's BPM
and play/pause state. The frame counter then runs locally, because a 2x
follower necessarily advances through frames twice as fast. If the master
exits, a follower takes over.

For instances on different machines, or to avoid coordinating ports, use a
multicast group instead; every instance then finds the others automatically:

```sh
build/orca --sync-multicast 239.255.0.1:9999
```

`--sync` binds the given port for the first instance and walks upward
(`9999`, `10000`, ...) for each additional one, since two sockets sharing one
port would have their incoming traffic divided between them.

## Tempo multipliers

Each instance has a local tempo multiplier applied on top of the shared BPM.
The master's BPM is unaffected, so instances can run at different speeds
against the same beat without fighting over the clock.

Set it at startup with `--rate` (a whole number or a fraction), change it while
playing with `F2`–`F7`, or pick from `Clock & Timing -> Tempo Multiplier`:

| Key | Menu | Multiplier |
|---|---|---|
| `F2` | 1/4 speed (quarter time) | 1/4 |
| `F3` | 1/2 speed | 1/2 |
| `F4` | Normal speed (1x) | 1 |
| `F5` | Double speed (2x) | 2 |
| `F6` | Triple speed (3x) | 3 |
| `F7` | Quadruple speed (4x) | 4 |

The current multiplier is shown next to the BPM in the HUD, for example
`120 2x`. Note durations scale with it, since they are derived from the frame
length.

Switching multipliers keeps the current beat phase: the fraction of the frame
already elapsed is rescaled to the new frame length, so fast F-key changes stay
on the grid instead of pushing the next beat out each time.

## Extras

- Discuss and get help in the [forum thread](https://llllllll.co/t/orca-live-coding-tool/17689).
- Support this project through [Patreon](https://patreon.com/100).
- See the [License](LICENSE.md) (MIT) file for license rights and limitations.
