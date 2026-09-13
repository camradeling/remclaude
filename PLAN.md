# remclaude — remote Claude Code session server

Wraps the `claude` CLI as a subprocess behind a small TCP service, so a prompt
sent from another host (over a private VPN) can drive a named, resumable
Claude Code conversation and get the reply back.

## Threat model / assumptions

- Both hosts are on a single-user VPN; the socket binds only to the VPN
  interface, never `0.0.0.0`.
- No TLS in the base design — the VPN tunnel is the confidentiality/integrity
  boundary. A shared token guards against misconfiguration or a stray second
  device on the VPN, not against a hostile network.
- `--dangerously-skip-permissions` is used so prompts run fully unattended
  (no TTY to answer permission prompts). This means any prompt sent to the
  server can execute shell commands / edit files with no confirmation step —
  acceptable only because the client side is trusted (you, over your own
  VPN). Treat the token and the VPN as the only things standing between a
  sender and full tool execution on the server host.

## Protocol (Phase 1)

Newline-delimited JSON, one object per line, over a raw TCP socket.

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

## Phases

### Phase 1 — Core server + client (this delivery)

- `server.cpp`: POSIX socket server, thread-per-connection, shared-token
  check, JSON-file-backed session registry (name ↔ Claude session UUID),
  fork/exec wrapper around `claude -p ... --output-format json` (no shell,
  so prompt text can't be interpreted as shell syntax), per-session mutex
  so two overlapping prompts to the same session can't race on one
  transcript file.
  - One-time startup calibration call discovers which
    `~/.claude/projects/<encoded-cwd>/` bucket this server's cwd maps to,
    instead of reimplementing Claude Code's internal path-encoding scheme.
  - `--session-id <uuid>` on a session's first prompt, `--resume <uuid>` on
    every prompt after — both confirmed empirically against the real CLI.
  - Deleting a session removes it from the registry **and** deletes the
    underlying `<uuid>.jsonl` transcript (per your answer earlier).
- `client.cpp`: connects, on start fetches the session list and shows a
  menu (pick existing / new / delete), then drops into a prompt loop for
  the chosen session; `:menu` to switch sessions, `:quit` to exit.
- Manual run (no service manager yet): start `server` bound to the VPN
  interface IP on the host machine, start `client` pointed at that IP from
  the other host.

### Phase 2 — Hardening & running as an actual service

Not yet implemented. Planned work:

- Logging: request/response audit trail (timestamps, session name, prompt
  length, cost, exit code) to a file — no prompt/response *content* in logs
  by default, since that's sensitive by nature.
- Config file instead of positional CLI args (bind IP, port, token,
  registry path, project workdir) so secrets aren't visible in `ps`.
- `systemd` unit (`remclaude.service`) so the server survives reboots and
  restarts on crash; `WantedBy=multi-user.target`, `Restart=on-failure`.
- Idle/orphan cleanup: optional max session count or max-age eviction so the
  registry and transcript directory don't grow unbounded.
- Basic connection hygiene: read/write timeouts, max line length (guard
  against a client that never sends `\n`), max concurrent connections.

### Phase 3 — TLS + REST API (**implement later, not in scope now**)

Planned but deliberately deferred — current VPN-only + shared-token model is
considered sufficient for now. Revisit if:
- the server ever needs to be reachable outside the VPN, or
- multiple client devices/tools need to integrate without running the
  custom NDJSON client, or
- you want the token replaced with something revocable/rotatable per client.

Planned shape when this phase is picked up:
- Swap the raw-socket NDJSON transport for a real HTTP(S) server (e.g.
  `cpp-httplib` or similar single-header lib, to stay dependency-light) so
  the same `list` / `create` / `delete` / `prompt` operations become REST
  endpoints (`GET /sessions`, `POST /sessions`, `DELETE /sessions/{name}`,
  `POST /sessions/{name}/prompt`).
  If a native REST API is too much surface, an intermediate step is
  fronting the existing NDJSON server with `stunnel`/`nginx` for TLS
  termination only, without changing the protocol.
- TLS: server certificate (self-signed or private CA is fine for a
  single-user setup) terminated either natively (OpenSSL/BoringSSL linked
  into the server) or via a reverse proxy in front of it.
- Auth: move off the single static shared token toward per-client
  bearer tokens (so a client can be revoked individually), still checked
  over the now-encrypted channel.
- Decide at that point whether the VPN requirement is dropped (if TLS +
  per-client auth is judged sufficient on its own) or kept as defense in
  depth.

## Non-goals (for now)

- Multi-user / multi-tenant support.
- Streaming partial output back to the client (current design waits for
  the full `claude -p` result before replying).
- Any web UI — client is a terminal menu.
