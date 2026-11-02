---
name: buildapk
description: Build and sign the remclaude Android APK (gui/ subproject, arm64-v8a), optionally install it via adb.
argument-hint: [install]
user-invocable: true
allowed-tools: Bash
---

## Step 1 — Prerequisites

Run all commands from the repository root. Paths default to `$HOME`; export
`QT_ROOT` (folder holding `6.11.2/`) and `ANDROID_SDK_ROOT` if they live elsewhere.

- Qt for Android: `$QT_ROOT/6.11.2/android_arm64_v8a` (host Qt: `.../gcc_64`).
- SDK `$ANDROID_SDK_ROOT`, newest NDK under `ndk/`, build-tools 36.1.0.
- Submodule: `git submodule update --init --depth 1`
  (KDAB android_openssl; libcrypto is linked statically).

## Step 2 — Configure (only if `gui/build-android/CMakeCache.txt` is missing)

The build dir lives inside the repo (gitignored) so it never pollutes the parent directory.
```bash
QT_ROOT=${QT_ROOT:-$HOME/Qt}; SDK=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
QTA=$QT_ROOT/6.11.2/android_arm64_v8a
NDK=$(ls -d $SDK/ndk/* | sort -V | tail -1)
ANDROID_NDK_ROOT=$NDK $QTA/bin/qt-cmake -S gui -B gui/build-android -DCMAKE_BUILD_TYPE=Release \
  -DANDROID_SDK_ROOT=$SDK -DQT_ANDROID_ABIS=arm64-v8a -DQT_HOST_PATH=$QT_ROOT/6.11.2/gcc_64
```

## Step 3 — Build

```bash
cmake --build gui/build-android --target remclaude_gui_android_make_apk -- -j$(nproc) 2>&1 | grep -v riscv64
```
On failure show the last 40 lines and stop.

## Step 4 — Sign (release output is unsigned)

```bash
BT=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/build-tools/36.1.0; KS=~/.android/debug.keystore
OUT=gui/build-android/android/android-build/build/outputs/apk/release
$BT/zipalign -f -p 4 $OUT/android-build-release-unsigned.apk $OUT/aligned.apk
$BT/apksigner sign --ks $KS --ks-pass pass:android --out gui/build-android/remclaude.apk $OUT/aligned.apk
```
If the keystore is missing, create it with `keytool -genkeypair` (alias `androiddebugkey`, pass `android`).

## Step 5 — Install (if $ARGUMENTS contains `install`)

`adb install -r gui/build-android/remclaude.apk`. The key and token go to
`/sdcard/Android/data/org.camradeling.remclaude/files/` (`id_ed25519`, `token.txt`). Launch
the app once first so the folder exists.
