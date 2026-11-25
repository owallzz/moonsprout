# mppm-server

The lobby, signalling and relay server for mppm online play: one small Rust binary (tokio and
axum). Players host the game sessions themselves; the server only

- hands out **join codes** (`ABCD-EFGH`) and keeps the member list of each session,
- forwards **signalling** messages between the host and each member over a WebSocket,
- hands out **ICE servers** (STUN, and TURN with fixed or minted credentials), or
- runs a **builtin UDP relay** (`--relay`) for players whose direct connection fails.

It runs no game logic and stores nothing on disk. The wire protocol is in
[PROTOCOL.md](PROTOCOL.md); the client is `port/net/lobby_client.cpp` in the game.

## Build

Rust 1.85 or newer:

```sh
cd server
cargo build --release --locked      # target/release/mppm-server
cargo test --locked                 # unit and integration tests (they start the binary on 127.0.0.1)
cargo clippy --locked --all-targets -- -D warnings
```

`Cargo.lock` is committed; `--locked` builds exactly those dependency versions. The binary has no
TLS: put it behind a TLS proxy (Caddy, the Fly.io edge) for anything but local tests.

## Run

```sh
# Local test server (loopback only, the default bind), builtin relay on, ports printed to a file:
target/release/mppm-server --bind 127.0.0.1:0 --port-file /tmp/mppm-server.port --relay --relay-port 0
```

When it is ready it prints one line on stderr (stdout stays empty):

```
mppm-server: listening http=127.0.0.1:47200 relay=127.0.0.1:47201 ports=41000-41999 mode=builtin
```

(`relay=- ports=-` when the builtin relay is off). SIGINT or SIGTERM sends every member
`closed {reason:"server_shutdown"}`, logs the open relay mappings and exits 0. Bad flags exit 2.

In the game, point **Settings > Online > Server** (`online.serverUrl`) or `--server` at
`http://127.0.0.1:<port>`. Plain `http://` is accepted only for 127.0.0.0/8, `::1` and `localhost`;
any other server must be `https://` (the game connects to `wss://<host>/v1/ws`).

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--bind ADDR:PORT` | `127.0.0.1:8080` | HTTP and WebSocket listen address. Port 0 picks a free port. |
| `--port-file PATH` | - | Once listening, write the HTTP port (and `relay=<port>` on a second line). |
| `--log-file PATH` | - | Append log lines to this file as well as stderr. |
| `--relay-mode MODE` | `$MPPM_RELAY_MODE`, else `none` | `none`, `static`, `shared-secret`, `builtin` or `cloudflare` (see below). |
| `--relay` | - | Same as `--relay-mode builtin`. |
| `--stun URL` | - | STUN server for clients, repeatable (`stun:host:3478`). |
| `--turn URL` | - | TURN server (`turn:host:3478`) for `static` and `shared-secret`. |
| `--turn-user U`, `--turn-pass P` | - | Fixed TURN credentials (`static`). |
| `--turn-secret S` | `$MPPM_TURN_SECRET` | coturn `static-auth-secret` (`shared-secret`). Never sent to clients. |
| `--turn-ttl SECONDS` | `21600` | Lifetime of minted TURN credentials. |
| `--relay-bind IP` | `127.0.0.1` | Builtin relay: address of the control port and member ports (IPv4). |
| `--relay-port PORT` | `3479` | Builtin relay: UDP control port (0 = any). |
| `--relay-ports LO-HI` | `41000-41999` | Builtin relay: member ports. |
| `--relay-public IP` | - | Builtin relay: address announced to clients (behind NAT or Docker). |
| `--relay-kbps N` | `2048` | Builtin relay: cap per member port and direction, kbit/s. |
| `--session-ttl SECONDS` | `21600` | Hard lifetime of a session. |
| `--max-sessions N` | `2000` | Live sessions on this server. |
| `--max-members N` | `8` | Members per session, the host included (2..8). |
| `--limits-scale F` | `1.0` | Multiplies every per-address rate limit (tests and LAN parties). |
| `--trust-proxy` | - | Take the client address from `Fly-Client-IP` / `X-Forwarded-For`. Only behind a proxy you control. |

Logging: `RUST_LOG` (for example `RUST_LOG=mppm_server=debug`) changes the detail; the default logs
sessions starting and ending, never join codes or secrets.

### Relay modes

- `none`: clients get only the `--stun` servers and connect directly (or not at all).
- `static`: a TURN server with one fixed username and password, for a private server.
- `shared-secret`: coturn with `use-auth-secret`. The server mints a credential per member
  (`username = <expiry>:<member>`, `credential = base64(HMAC-SHA1(secret, username))`) that coturn
  checks with the same secret. Use this for a public self-hosted server (see `docker-compose.yml`).
- `builtin`: this binary relays UDP itself (PROTOCOL.md, "Builtin relay"). No coturn needed; open
  the control port and the member port range. Each relayed player uses about 12 KB/s.
- `cloudflare`: Cloudflare Realtime TURN credentials. **Not available yet** (it waits for a
  compatibility test of libjuice with Cloudflare TURN); the server refuses to start with it.

## Self-hosting

The repository ships three files for running your own server. They are examples: nothing builds or
deploys them automatically.

- [`Dockerfile`](Dockerfile): a static musl binary in a small Alpine image, running as an
  unprivileged user. `docker build -t mppm-server server/`.
- [`docker-compose.yml`](docker-compose.yml): `mppm-server` in `shared-secret` mode next to coturn,
  sharing one `TURN_SECRET` from a `.env` file next to it:

  ```sh
  cd server
  printf 'PUBLIC_HOST=play.example.org\nTURN_SECRET=%s\n' "$(openssl rand -hex 32)" > .env
  docker compose up -d
  ```

  Open TCP 8080 (or the TLS proxy's 443), UDP and TCP 3478 and UDP 49152-65535 for coturn. Uncomment
  the Caddy service for automatic HTTPS, then use `https://play.example.org` as the game's server URL.
- [`fly.toml`](fly.toml): the official lobby, `moonsprout-lobby` on Fly.io (one machine that stops
  when idle; forks pick their own app name, `fly launch --copy-config --no-deploy`, then
  `fly deploy --ha=false`). Fly
  terminates TLS. UDP relaying on Fly needs a dedicated IPv4 address and is not configured here; use
  `shared-secret` with an external coturn, or `builtin` on a VPS.

For the builtin relay on a VPS without Docker:

```sh
mppm-server --bind 0.0.0.0:8080 --relay --relay-bind 0.0.0.0 --relay-public <public IPv4> \
            --relay-port 3479 --relay-ports 41000-41999
```

Every STUN and TURN address the game uses comes from the server it talks to, so a fork only changes
the game's server URL. Join codes live on the server that created them.

## Code map

| File | What it holds |
|---|---|
| `src/main.rs` | Start-up: flags, logging, the listener and `--port-file`, the builtin relay's control task, a 250 ms timer that reaps expired sessions (and sweeps the rate limits every 60 s), SIGINT/SIGTERM shutdown. |
| `src/config.rs` | The command line (`clap`, with `MPPM_TURN_SECRET` and `MPPM_RELAY_MODE` from the environment) and its checks (bad flags exit 2). |
| `src/codes.rs` | Join codes (8 Crockford base32 symbols from the OS RNG, read in any case), member secrets and tokens, `sanitize_name`. |
| `src/session.rs` | Sessions and members, every state change (create, join, attach, signal, kick, leave, expiry) under one mutex (lock order `inner`, `limits`, relay); the client address for the limits (`--trust-proxy`). |
| `src/rest.rs` | HTTP routes: the REST endpoints, `/v1/health` and the WebSocket upgrade. |
| `src/ws.rs` | `GET /v1/ws`: one task per WebSocket, the client messages and the server events (PROTOCOL.md, "WebSocket protocol"). |
| `src/proto.rs` | Wire formats: the WebSocket JSON messages and the MPRL v1 relay datagrams. |
| `src/ice.rs` | The ICE server list a member gets for each relay mode (minted `shared-secret` credentials). |
| `src/relay.rs` | The builtin UDP relay (MPRL v1): the control port, one member port per client, the kbps cap. |
| `src/ratelimit.rs` | Token buckets and the per-address limits (scaled by `--limits-scale`). |
| `tests/common/mod.rs` | Test helpers: the real binary on 127.0.0.1, a WebSocket client, a small HTTP client. |
| `tests/rest.rs`, `tests/ws.rs`, `tests/relay.rs` | REST, attach, rate limits and ICE lists; the WebSocket flows; the relay with real UDP sockets (member ports 47000-47049). |
