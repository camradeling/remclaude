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
