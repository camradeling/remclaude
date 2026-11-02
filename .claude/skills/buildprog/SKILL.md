---
name: buildprog
description: Build the remclaude desktop GUI (Qt6/CMake, gui/ subproject).
argument-hint: [release|debug]
user-invocable: true
allowed-tools: Bash
---

Build the remclaude desktop GUI (`gui/`). The server and CLI client are built
separately with the top-level `Makefile`.

## Step 1 — Configure if needed

Run all commands from the repository root.


If `gui/build/CMakeCache.txt` does not exist, configure:
```
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=<Release|Debug>
```
Use `Debug` if $ARGUMENTS contains `debug`, otherwise `Release`. The first configure
fetches libssh2 from GitHub (needs network).

## Step 2 — Build

```
cmake --build gui/build -j$(nproc) 2>&1
```
On failure show the last 40 lines and stop.

## Step 3 — Report

Binary: `gui/build/desktop/remclaude_gui`. Report its path and size, and how to run it.
Key and token default to `~/.local/share/remclaude/{id_ed25519,token.txt}`.
