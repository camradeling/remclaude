# remclaude

Wraps the `claude` CLI as a subprocess behind a small TCP service, so a
prompt sent from another host (over a private VPN) can drive a named,
resumable Claude Code conversation and get the reply back.

See [`PLAN.md`](PLAN.md) for the design, protocol, and phase roadmap.

## Install

### Server host (the one with Claude Code + VPN)

1. **Prereqs**: `claude` CLI already installed and logged in (this is the
   account that will run the actual conversations), plus build tools:

   ```sh
   sudo apt install g++ nlohmann-json3-dev make
   ```

2. **Get the code**:

   ```sh
   git clone git@github.com:camradeling/remclaude.git
   cd remclaude
   make
   ```

3. **Configure**:

   ```sh
   cp config.example.json config.json
   openssl rand -hex 32   # paste this into config.json's "token"
   ```

   Edit `config.json`: set `bind_ip` to the **VPN interface's** IP (not
   `0.0.0.0`), pick a `port`, set `project_workdir` to wherever you want
   sessions bucketed, and point `registry_path`/`log_path` somewhere
   writable (a plain subdirectory is fine to start).

   ```sh
   chmod 600 config.json   # it holds the shared token
   ```

4. **Test it manually first**:

   ```sh
   ./server config.json
   ```

   Leave it running, confirm it prints `Listening on <vpn-ip>:<port>`.

5. **Install as a service** (once the manual run works): edit
   `remclaude.service` (`User`, `Group`, `WorkingDirectory`, `ExecStart`
   paths) to match your setup, then:

   ```sh
   sudo cp remclaude.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now remclaude
   sudo systemctl status remclaude
   journalctl -u remclaude -f   # watch it live
   ```

### Client host (the other machine on the VPN)

The client only talks to the socket — it doesn't need `claude` installed
at all.

1. **Prereqs**:

   ```sh
   sudo apt install g++ nlohmann-json3-dev make
   ```

2. **Get the code and build just the client** (or copy the whole repo,
   doesn't matter):

   ```sh
   git clone git@github.com:camradeling/remclaude.git
   cd remclaude
   make client
   ```

3. **Get the token onto this host** — don't paste it through chat; copy it
   over the VPN itself, e.g.:

   ```sh
   scp server-host-vpn-ip:/path/to/remclaude/config.json /tmp/cfg.json
   python3 -c "import json;print(json.load(open('/tmp/cfg.json'))['token'])" > token.txt
   rm /tmp/cfg.json
   ```

   or just retype it manually into a file if you'd rather not copy the
   whole config.

4. **Run it**:

   ```sh
   ./client <server-vpn-ip> <port> @token.txt
   ```

That's it — the client shows the session menu (pick existing / create /
delete) and then drops into a prompt loop for the chosen session. Use
`:menu` to switch sessions and `:quit` to exit.

### GUI client over SSH (Ubuntu desktop and Android)

`gui/` is a Qt6/QML app (UI adapted from confucius) that drives the same
server through an **SSH tunnel**. It logs in to the server host with an SSH
key, opens a `direct-tcpip` channel to the remclaude port, and speaks the
protocol below. The server code does not change.

1. **Server side**: the remclaude socket only has to be reachable from the
   SSH host itself, so you can set `"bind_ip": "127.0.0.1"` in
   `config.json` (keeping the VPN IP also works, if you set the matching
   address in the app). Add the app's **public** key to
   `~/.ssh/authorized_keys` of the user the app logs in as.

2. **Build** (desktop):

   ```sh
   sudo apt install cmake g++ libssl-dev qt6-base-dev qt6-declarative-dev \
        qml6-module-qtquick-controls qml6-module-qtquick-layouts \
        qml6-module-qtquick-window qml6-module-qtqml-workerscript
   cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release
   cmake --build gui/build -j$(nproc)
   ```

   libssh2 1.11.1 is fetched and built statically by CMake (needs network on
   the first configure).

3. **Put the private key and token in the app data folder**
   (`~/.local/share/remclaude/` by default; the Settings screen shows the
   path and whether each file is found):

   ```sh
   mkdir -p ~/.local/share/remclaude
   cp ~/.ssh/id_ed25519_remclaude ~/.local/share/remclaude/id_ed25519
   chmod 600 ~/.local/share/remclaude/id_ed25519
   # token: first line of token.txt (same file the CLI client uses with @token.txt)
   cp token.txt ~/.local/share/remclaude/token.txt
   ```

   The key must be in OpenSSH format **without a passphrase** (ed25519 or
   RSA).

4. **Run**: `gui/build/desktop/remclaude_gui`. In ⚙ Settings, enter the
   server's host/IP, SSH port, and username (the key and token paths are
   pre-filled), then press **Connect**. Use ☰ to create, pick, or delete
   sessions. ■ cancels a running prompt on the app side only: the server
   still finishes it.

Notes:
- The server's SSH host key is trusted on first use and stored in
  `<data dir>/known_hosts`. If the key later changes, the app refuses to
  connect and tells you which entry to remove.
- Chat history is kept **locally**, one JSON file per session in
  `<data dir>/history/`, because the server has no history command.
  Deleting a session in the app deletes its local history too.
- Settings are stored in `~/.config/remclaude/remclaude.conf`.

**Restrict the app's key** on the server to tunnelling only. `restrict`
on its own still lets the key run commands, so add a forced no-op command:

```
command="/bin/false",restrict,port-forwarding,permitopen="127.0.0.1:8765" ssh-ed25519 AAAA... remclaude-phone
```

#### Android

Requires Qt 6.x for Android (`android_arm64_v8a`, built against the host
`gcc_64` Qt), the Android SDK and NDK, and the KDAB OpenSSL submodule
(`git submodule update --init --depth 1`). libcrypto is linked statically,
so no `.so` files are bundled. See `.claude/skills/buildapk/SKILL.md` for
the exact configure, build, and sign commands. The output is
`gui/build-android/remclaude.apk` (arm64, package `org.camradeling.remclaude`).

```sh
adb install -r gui/build-android/remclaude.apk
# open the app once so its folder exists, then:
adb push id_ed25519 token.txt /sdcard/Android/data/org.camradeling.remclaude/files/
```

In the app: ⋮ → Settings → host/IP + user → Connect. Notes:
- The app's packets must actually reach the server. If the phone runs a
  VPN that captures LAN traffic, either turn it off, let it bypass the
  local network, or put both devices on the same VPN.
- adb over Wi-Fi from a second Linux account: run `adb tcpip 5555` from the
  account that owns the USB device, then use a separate adb server port
  (`ANDROID_ADB_SERVER_PORT=5038 adb connect <phone-ip>:5555`) so the two
  servers don't fight over port 5037.

## Protocol

Newline-delimited JSON, one object per line, over a raw TCP socket:

```
-> {"token":"...","cmd":"list"}
<- {"ok":true,"sessions":[{"name":...,"id":...,"created":...,"last_used":...}]}

-> {"token":"...","cmd":"create","name":"..."}
<- {"ok":true,"name":"...","id":"..."}

-> {"token":"...","cmd":"delete","name":"..."}
<- {"ok":true}

-> {"token":"...","cmd":"prompt","name":"...","text":"..."}
<- {"ok":true,"result":"...","is_error":false,"cost_usd":0.01}
<- {"ok":false,"error":"..."}   (any command, on failure)
```

## Security model

- Both hosts are assumed to be on a single-user VPN; the socket binds only
  to the VPN interface, never `0.0.0.0`.
- No TLS — the VPN tunnel is the confidentiality/integrity boundary. The
  shared token guards against misconfiguration or a stray second device on
  the VPN, not against a hostile network.
- The server runs `claude` with `--dangerously-skip-permissions`, so any
  prompt sent to it can execute shell commands / edit files with no
  confirmation step. Treat the token and the VPN as the only things
  standing between a sender and full tool execution on the server host.

TLS + a REST API are planned but deliberately deferred — see the Phase 3
section of `PLAN.md`.

## Pre-commit secret scan

The repo ships a pre-commit hook that blocks commits containing likely
secrets or private info: credential-like file names (`token.txt`,
`config.json`, `id_*` keys, `*.pem`, …), private keys, SSH public key
bodies, API tokens, long hex strings, JSON `"token"/"password"` values,
`/home/<user>/` paths, private IP addresses, and email addresses. It only
looks at what is being committed (staged file names and added lines).
Enable it once per clone:

```sh
git config core.hooksPath .githooks
```

For a false positive, add a narrow regex to `.githooks/secret-allowlist`.
To bypass on purpose: `git commit --no-verify`.
