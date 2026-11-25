# M5.3b part 1: lockstep netcode, local

Milestone M5.3b of [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md) (section 8, the second "Step 3"
row; design in sections 2.1 to 2.11, section 10 and 7b) turns the determinism harness into online
play: every machine runs the whole game, only controller input travels, and a lobby server joins the
players by code. **Part 1 is local only.** Everything here was built and is tested on one machine,
on 127.0.0.1, with the lobby server started by the test and network delay, jitter and loss
simulated inside the client. Nothing contacts Fly.io, Cloudflare or any other outside service, and
no account or real credential is used. The Cloudflare TURN spike, the Fly deployment and the
two-machine tests are part 2 or the user's (see [Deferred](#deferred)).

This page is the overview: what part 1 delivers, how to run a session by hand, every flag, setting,
log line and exit code, the impairment, the done test and its results, the deviations from the plan,
the known limits and what is left. The parts have their own pages:

| Page | What |
|---|---|
| [server/PROTOCOL.md](../../server/PROTOCOL.md) | the lobby server's WebSocket and REST protocol, ICE modes and the builtin relay (`MPRL`): the authority for clients |
| [server/README.md](../../server/README.md) | building, running and self-hosting `mppm-server` |
| [M5_3B_TRANSPORT.md](M5_3B_TRANSPORT.md) | the transport: ENet, the socket shim, ICE through libjuice, the relay client, the impairment, its tests |
| [M5_3B_LOCKSTEP.md](M5_3B_LOCKSTEP.md) | the session protocol (wire format, bundles, barriers, time sync, input delay, desync, drops, spectators), the seam and host changes, its self-test |
| [../launcher/SETTINGS.md](../launcher/SETTINGS.md) section 18, [../launcher/DESIGN.md](../launcher/DESIGN.md) "Online screens" | the Multiplayer window, the wait overlay, the end messages, the Settings ONLINE tab |
| [M5_1A_HARNESS.md](M5_1A_HARNESS.md) | the harness under all of it (sessions, ticks, `.mpnl` up to version 5, `--ch-stage`) |
| [M5_1B_HOOKS.md](M5_1B_HOOKS.md) | every game-side hook line, the M5.3b one (`vsGS_Title.cpp:192`) included |

## What part 1 delivers

| Plan item | Part 1 |
|---|---|
| 2.2 transport | ENet 1.3.18 on a net thread (immediate flush, a duplicate send half a tick later, throttling off on the input channel) under a socket shim with three ways to reach a peer: direct UDP, ICE through libjuice 1.7.4, the builtin relay. Both libraries come from pinned release tarballs (CMake `FetchContent` with SHA-256). An in-process impairment stage (delay, jitter, loss, duplication) runs only when a flag or variable asks for it. |
| 2.2 topology, delay | Star through the host with forward-on-complete, 8-tick redundancy (up to 32 when a peer catches up), a per-sender input delay `d_i` from measured one-way delays (floor 1, cap 8), raised at once, lowered at barriers. |
| 2.3 session flow | Lobby on `mppm-server` (create, join by code, member list, signalling); the lobby room (seats, teams, ready, mode, stage, handicaps) kept by the host and sent over ENet; Hello with `build_hash`, `content_hash` and the mod list (7b); `START` with the seed and the host's Synced settings; start and load barriers over the network. |
| 2.4 seam and stall loop | The online branch of `p2_net_exchange`; the late-sampling wait waits for tick T+1 and turns into a hold loop with the "Waiting for P3…" overlay when it is late; the same hold as a fallback in the seam. |
| 2.6 settings lock | `net_lock_settings` with the host's Synced values at `START`, unlocked at the end. |
| 2.7 desync | A checksum every 30 ticks in the input packets, compared by the host; on a mismatch every peer writes a report and the session ends. |
| 2.8 disconnects, spectators | A dropped seat gets neutral input from a host-chosen tick on and the match goes on; spectators join in the lobby and simulate the bundle stream. |
| 2.9 host events | `{tick, id, arg, seq}` records in the host's bundles (drop notices in part 1). |
| 2.10 items 2, 3 | The net transport above; time sync with one hard jump at each load barrier, a fixed online tick grid that catches up long ticks, whole-tick grid shifts for large phase errors and a slew of at most 1 ms per retrace for the rest (D11). |
| 2.11 servers | `server/`, the Rust crate `mppm-server` (tokio, axum): REST and WebSocket, 40-bit codes, rate limits, ICE modes none/static/shared-secret, the builtin UDP relay; `Dockerfile`, `docker-compose.yml` and `fly.toml` as files only. The `online.serverUrl` setting and `--server`. |
| section 10 | Slot ownership, bundles only from the host, fixed packet bounds, tick-window checks, session tags, malformed packets drop the sender, relay caps, join secrets, rate limits, plaintext only on loopback. |
| Tier 0 | Online 2P Battle and 2P Challenge (the lobby picks the stage or course; a new `p2_net_ch_inject` hook), and 4-peer 2v2 Battle on the M5.3a captains. |
| UI | Moonsprout start menu entry **Multiplayer** (host, join by code, lobby room, ready, mode and stage, start), the in-game wait overlay with **Drop**, the end messages, the in-game **Online** bar entry and the Settings **ONLINE** tab. |

## Architecture

```
 game thread (cooperative game scheduler)               net thread (std::thread, port/net/session)
 ─────────────────────────────────────────              ──────────────────────────────────────────
 JUTGamePad::read -> p2_net_exchange  ──LocalInput──▶  SPSC ─▶ ClientCore / HostCore (pure)
   (online: sample port 0, wait ready,  ──TickDue───▶          wire encode/decode, bundles,
    copy bundle, record)                ──Checksum──▶          barriers, sync, delay, desync,
 p2_net_nonsim_leave (depth 0)          ──Barrier───▶          drops, spectators, lobby room
 p2_host_present -> pre_read_wait ◀──Bundle/Release── SPSC     │            │
   (wait due-m and ready, hold loop)  ◀──events/status──       ▼            ▼
 p2_net_game_poll (START, END)                            Transport     LobbyClient
 launcher (UI) -> mppm::online::* ──commands (MPSC)──▶   (ENet+shim)   (borealis::ws)
                ◀── snapshot (mutex copy) ───                │              │
 os_thread timerMain: nextRetrace += P + clamp(slew, ±1 ms) ◀ p2_vi_phase_slew_set / grid_jump
                                                              ▼              ▼
                                              UDP / libjuice agents / relay   mppm-server
```

| Layer | Files | Role |
|---|---|---|
| Lobby server | `server/**` (Rust) | membership, codes, signalling, ICE credentials, the builtin relay; it knows no seats and runs no game logic |
| Lobby client | `port/net/lobby_client.{hpp,cpp}` | one WebSocket (`borealis::ws`) to the server for create, join, members and signalling |
| Transport | `port/net/transport.hpp`, `port/net/transport/**` | `mppm::net::Transport`: ENet over the shim `enet_juice.c` (direct, ICE, relay), the impairment |
| Session | `port/net/session/**` | the net thread, the pure host and client cores, the wire format, the connector, the façade `mppm::online` (`port/include/p2_online.hpp`), the in-memory transport and the test bots |
| Seam and host | `port/net/net.cpp`, `port/main.cpp`, `port/sdk/os_thread.cpp`, `port/game/net/**` | the online mode of the harness machinery: input exchange, readiness, hold loop, phase slew, grid jump, the Challenge inject |
| Launcher | `port/launcher/**` | the online windows over `mppm::online`, the settings hooks |

One tick on a client (update-first, late sampling on):

1. End of pass t, `p2_host_present`: present, then `pre_read_wait`: sleep until `due(t+1) - 1 ms`; if
   tick t+1 is not ready by `due(t+1)`, count a stall and hold (overlay after 300 ms) until it is.
2. Pass t+1, `JUTGamePad::read` → `p2_net_exchange`: pack physical port 0 (or the `--net-input`
   record) as tag `t+1+d`, hand it to the net thread (sent at once with a flush and again 16.7 ms
   later); if tick t+1 is still not ready, hold (fallback); write bundle t+1 into all four
   `PADStatus` slots; record; next tick.
3. The pacing wait returns at once (the retrace has almost passed); update, draw, present.

The host runs the same loop. Its net thread completes bundle X the moment the last seat's input for
X arrives, sends it to every client and spectator and hands it to its own game thread.

## Running a local session by hand

Build (in the distrobox for the game, on the host for the server):

```sh
distrobox enter mppm-build -- bash -lc 'cd /path/to/moonsprout && cmake build-pc >/dev/null && ninja -C build-pc mppm p2net_selftest p2transport_test p2net_turn p2lobby_test p2libm_selftest'
(cd server && ~/.cargo/bin/cargo build --release --locked)
```

Start a lobby server with the builtin relay, bound to loopback:

```sh
server/target/release/mppm-server --bind 127.0.0.1:47200 --relay --relay-bind 127.0.0.1 \
    --relay-port 47201 --relay-ports 47210-47299
```

It prints `mppm-server: listening http=127.0.0.1:47200 relay=127.0.0.1:47201 ports=47210-47299 mode=builtin`
on stderr when ready. Then a host and a client, each with its own home directory and window (under
Xvfb here; on a desktop drop the `xvfb-run` part and the headless variables):

```sh
H=/var/tmp/mppm-online/hand
python3 tools/net/netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0 --ticks 3600 -o $H/S2.mpnl
for p in host client; do mkdir -p $H/$p/.local/share; done
run() { # run NAME DISPLAY ARGS...
    local n=$1 d=$2; shift 2
    env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR GDK_BACKEND=x11 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
        SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy HOME=$H/$n XDG_DATA_HOME=$H/$n/.local/share \
        MPPM_SKIP_LAUNCHER=1 MPPM_WINDOW=640x480 \
        xvfb-run -n $d -s "-screen 0 1280x960x24" ./build-pc/mppm "$MPPM_TEST_DISC" \
        --server http://127.0.0.1:47200 --net-bind 127.0.0.1 --cvar game.idleDemo=false "$@" >$H/$n.log 2>&1
}
run host 72 --host --ready --slot 0 --code-file $H/code --mode battle --stage 0 \
    --net-input $H/S2.mpnl --checksum-log $H/host.cksum --record $H/host.mpnl --exit-after-ticks 3600 &
while [ ! -s $H/code ]; do sleep 0.5; done
run client 73 --join "$(cat $H/code)" --slot 1 --ready \
    --net-input $H/S2.mpnl --checksum-log $H/client.cksum --record $H/client.mpnl --exit-after-ticks 3600 &
wait
cmp $H/host.cksum $H/client.cksum && python3 tools/net/netstats.py summary $H/host.log $H/client.log
```

- `--net-input` makes each player play its own seat of a synthetic log (the keyboard drives only
  port 0, and there is nobody to press keys under Xvfb). Without it the player's physical port 0
  (keyboard or first pad) is its seat.
- Add `--net-impair delay=46ms,jitter=8ms,seed=1` (and `seed=2` on the client) for a 100 ms
  round trip with jitter, `--net-mode relay` on both to go through the relay, `--net-mode ice` for
  ICE. A spectator joins with `--join CODE --spectate`; give the host `--expect 3` so it waits.
- The recording replays offline:
  `./build-pc/mppm "$MPPM_TEST_DISC" --replay $H/host.mpnl --checksum-verify $H/host.cksum --headless-ticks`.
- From the UI: set **Settings › Online › Lobby server** to `http://127.0.0.1:47200` (loopback
  plaintext is allowed), open **Multiplayer** on the start menu, **Host a game** on one machine and
  **Join with a code** on the other.

## Command line and environment

Flags win over the environment; `--flag value` and `--flag=value` both work. Every online flag is
read only at start-up.

| Flag | Environment | Meaning |
|---|---|---|
| `--server URL` | `MPPM_SERVER_URL` | lobby server (overrides the `online.serverUrl` setting). `https://` maps to `wss://…/v1/ws`; `http://` (plain `ws://`) only for 127.0.0.0/8, `::1` and `localhost` |
| `--host` | - | create a lobby at start-up (needs a server) |
| `--join CODE` | - | join that lobby at start-up (`ABCD-EFGH`; any case, spaces and hyphens ignored, `I`/`L` read as 1, `O` as 0) |
| `--spectate` | - | with `--join`: watch as a spectator |
| `--name TEXT` | `MPPM_NET_NAME` | player name, 1 to 24 characters (overrides `online.playerName`) |
| `--password TEXT` | - | lobby password (host: set it; member: give it) |
| `--mode battle\|2v2\|challenge` | - | host: the mode (default battle) |
| `--stage N` | - | host: VS stage or Challenge course (default 0) |
| `--handicap R,B` | - | host, Battle: handicaps 1 to 10 of Red and Blue (default stock) |
| `--slot N` | - | ask for seat N (0 to 3) |
| `--ready` | - | mark ready once seated; the host also starts as soon as every seat is taken and ready (and `--expect` is met) |
| `--expect N` | - | host with `--ready`: also wait until N members (host and spectators included) are connected (default: the seat count) |
| `--code-file PATH` | - | host: write the code (`ABCD-EFGH` and a newline) when the lobby exists |
| `--net-input FILE` | `MPPM_NET_INPUT` | local input from this seat's records of an `.mpnl` log (record t-1 at read t; neutral after the log ends) |
| `--net-impair SPEC` | `MPPM_NET_IMPAIR` | send-side impairment, tests only (see [Impairment](#impairment)) |
| `--net-mode auto\|direct\|ice\|relay` | `MPPM_NET_MODE` | connection order (D6); `MPPM_NET_FORCE_RELAY=1` = relay |
| `--net-bind ADDR` | `MPPM_NET_BIND` | UDP bind address (tests: 127.0.0.1) |
| `--net-allow-plaintext` | `MPPM_NET_ALLOW_PLAINTEXT=1` | allow `ws://` to a server that is not on loopback (D27) |
| `--net-test-bots N` | - | host without a server: N in-process bots take seats 1 to N (D29) |
| `--ch-stage N` | `MPPM_CH_STAGE` | harness: boot straight into 2P Challenge course N (0 to 29), a version 5 recording (D25) |
| | `MPPM_NET_ICE_RELAY_ONLY=1` | ICE with relay candidates only (tests) |
| | `MPPM_NET_TEST_DESYNC=T` | perturb this peer's SIM RNG at tick T (D23) |
| | `MPPM_NET_TEST_LEAVE_AT=T` | leave gracefully after tick T (BYE, `session end reason=left`, exit 0) |
| | `MPPM_NET_TEST_HANG_AT=T[:MS]` | the game thread sleeps MS ms (default 120000, at most 600000) after tick T while the net thread runs on: a stuck game for the host's progress rule |
| | `MPPM_NET_TEST_BOT_LOAD_MS=N` | bots report each barrier N ms late |
| | `MPPM_NET_TEST_SLEW_NS=N` | call `p2_vi_phase_slew_set(N)` once at the first session tick (harness runs too) |

An online run prints `mppm: net: online (server=<url>, role=host|join|bots, mode=..., net-mode=..., bind=...)`
at start-up. `--host` together with `--vs-stage` or `--ch-stage`, and `--replay` or
`--headless-ticks` with `--host`/`--join`, exit 2 (online sessions run in real time). The harness
flags `--record`, `--checksum-log`, `--checksum-verify` and `--exit-after-ticks` keep their meaning
in an online run.

**Settings** (Settings › ONLINE, [SETTINGS.md](../launcher/SETTINGS.md) section 18):
`online.serverUrl` (alias `online.server_url`, the plan's spelling; environment `MPPM_SERVER_URL`) and
`online.playerName`. The default server is the CMake cache string `MPPM_DEFAULT_SERVER_URL`, the
official lobby `https://moonsprout-lobby.fly.dev` (Fly.io, `server/fly.toml`: one machine that stops
when idle). Configure with `-DMPPM_DEFAULT_SERVER_URL=` (empty) for a build without a default: its
Multiplayer window says "No lobby server is set. Choose one in Settings › Online."

**Server** (`mppm-server --help`, [server/README.md](../../server/README.md)):
`--bind` (default `127.0.0.1:8080`), `--port-file`, `--log-file`, `--relay-mode none|static|shared-secret|builtin|cloudflare`
(`--relay` = builtin; `cloudflare` refuses to start in part 1), `--stun`, `--turn`, `--turn-user`,
`--turn-pass`, `--turn-secret` (or `MPPM_TURN_SECRET`), `--turn-ttl`, `--relay-bind`, `--relay-port`,
`--relay-ports`, `--relay-public`, `--relay-kbps`, `--session-ttl`, `--max-sessions`, `--max-members`,
`--limits-scale`, `--trust-proxy`. It binds loopback unless told otherwise.

## Log lines

Every line of the game starts with `mppm: net: ` (stderr). `tools/net/netstats.py` parses them.

| Line | Printed by | When |
|---|---|---|
| `online (server=<url>, role=host\|join\|bots, mode=<m>, net-mode=<n>, bind=<a>)` | every online peer | start-up |
| `impairment delay_us=<n> jitter_us=<n> loss_ppm=<n> dup_ppm=<n> seed=<n> (test only)` | transport | once, when impairment is on |
| `lobby created code=ABCD-EFGH server=<url>` | host | the lobby exists |
| `lobby failed error=<code> msg=<text>` | any | lobby failure (exit 9) |
| `peer <member> connected link=direct\|ice\|relay remote=<addr>` | host and client | a transport peer connects |
| `peer <member> disconnected reason=<text>` | host and client | it disconnects |
| `ice connected peer=<id> local=<type> remote=<type>` | transport | an ICE agent connects; types `host`, `srflx`, `prflx`, `relay` |
| `delay slot=<s> d=<d> path_us=<p> reason=init\|path\|stall\|boundary` (`... capped`) | each player | its input delay changes |
| `barrier tick=<B> waited_ms=<ms> jump_us=<signed us>` | every peer | a load barrier is released |
| `sync tick=<T> offset_us=<o> rtt_us=<r> phase_us=<e> slew_us=<remaining>` | clients and spectators | once per second |
| `phase shift ticks=<n> rest_us=<us>` | clients and spectators | a phase correction of a whole tick or more moves the tick grid (n > 0: earlier) |
| `stall window_ticks=900 stalled=<k> rate=<pct> max_ms=<ms> barrier_ms=<ms>` | every peer | every 900 ticks |
| `drop slot=<s> tick=<E> reason=<r>` | every peer | a seat is dropped; neutral input from tick E on |
| `desync at tick <T> (slot <s>: host <8 hex> peer <8 hex>); report <path>` | every peer | a checksum mismatch |
| `session end reason=done\|desync\|host_left\|dropped\|left\|error` | every peer | the session ends |
| `summary ticks=<T> stalled=<k> rate=<pct> max_ms=<ms> barriers=<n> barrier_ms=<ms> phase_p50_us=<n> phase_p99_us=<n> phase_max_us=<n> d=<seat>:<d>,... d_max=<n> rtt_us=<n> kbps_out=<n> kbps_in=<n> link=<direct\|ice\|relay\|host>` | every peer | after `session end` |

A tick is **stalled** when its read had to wait past its due time; barrier waits are counted apart.
The phase figures (`phase_p50_us`, `phase_p99_us`, `phase_max_us`, the figure the plan's "phase
error <= 2 ms p99" is read from) use samples taken at least 1 s after the newest barrier release and
leave out samples taken while more than 2 ms of correction was pending (the catch-up after a long
tick); the host prints `-1` (it does not slew). The server prints `mppm-server: listening ...` when ready and
`mppm-server: relay mapping session=<id> member=<m> port=<R> up_bytes=<n> down_bytes=<n> closed=<idle|session|shutdown>`
for every relay mapping it frees (the done test checks that bytes went through).

## Exit codes

For command-line online runs (`--host`, `--join`, `--net-test-bots`); the harness codes of
[M5_1A_HARNESS.md](M5_1A_HARNESS.md#exit-codes) still apply to everything else.

| Code | When | Line |
|---|---|---|
| 0 | `--exit-after-ticks` reached, the window was closed, or `MPPM_NET_TEST_LEAVE_AT` | `session end reason=done` (or `left`) |
| 2 | bad or conflicting flags | the reason |
| 7 | desync | `desync at tick ...`, then `session end reason=desync` |
| 8 | the host left, this seat was dropped, the connection was lost | `session end reason=host_left\|dropped\|error` |
| 9 | lobby failure: server unreachable, refused, a mismatch, connect timeout | `lobby failed error=<code> msg=<text>` |

In the UI a desync or a lost host ends the session on every machine with a message over the frozen
picture; **Back to start menu** restarts into the start menu (D21).

## Impairment

`tc netem` needs root, so the delay, jitter and loss of the tests are simulated in the client: the
transport's socket shim holds each outgoing datagram (both channels and ENet's own datagrams) in a
heap and sends it at `now + delay + U(0, jitter)` (reordering allowed); loss and duplication are
drawn per datagram from a seeded xorshift32. Only the sending side is impaired, on every peer, which
gives both directions and leaves the receiver unchanged.

`--net-impair SPEC` / `MPPM_NET_IMPAIR=SPEC`, SPEC a comma list of `delay`, `jitter`, `loss`, `dup`,
`seed` with units `us`, `ms`, `s`, `%`, `ppm` (for example `delay=46ms,jitter=8ms,loss=2%,seed=3`).
It is off unless given, logs one line when on, and counts dropped datagrams in the peer statistics.
The done test's WAN profile `delay=46ms,jitter=8ms` on both peers gives a mean round trip of 100 ms
between host and client (2 x 46 ms + the mean jitter of 2 x 4 ms). The transport's own test requires
an echo RTT of 100 ± 3 ms for `delay=50ms` each way; the design's feasibility experiment (ENet with a
delay-queue shim) measured 100.5 to 101 ms.

## Recordings

Online sessions always record the confirmed input stream: to `--record FILE` if given, else
`<userdir>/net/sessions/<code>-<yyyymmdd-hhmmss>.mpnl` (the 5 newest are kept). The file is version
3 (version 4 with 4 seats) with flags bit 1 (online) set; version 5 only for Challenge or non-stock
handicaps ([M5_1A_HARNESS.md](M5_1A_HARNESS.md#input-log-mpnl-binary-little-endian)). Replaying it
offline reproduces the session's checksum log, which the done test checks for Battle, 2v2, Challenge
and the loss run. Desync reports go to `<userdir>/net/reports/desync-<code>-t<tick>.txt` (build and
content hashes, mod list, seed, SessionConfig, seat, the last 64 checksum lines, the host's line for
the tick, the recording's path).

## Done test

```sh
MPPM_TEST_DISC=/path/to/your/disc.iso tools/net/m53b_test.sh [-b build-pc] [-d 72] [-o /var/tmp/mppm-online/m53b] \
    [-B aa6b6985e] [-q] [-P] [-k] [-c CHECK,...] [-l] [--dry-run]
```

Run from the repo root on the host. `-d D` uses Xvfb displays D to D+7; `-q` is quick (3600-tick
`bat2`, `bat4` and `ch2`, `m53a_test.sh -q`); `-P` runs `bat2` and `bat4` at the same time; `-k`
skips the build check; `-c` picks checks; `-l` lists them; `--dry-run` prints every command line the
selected checks would run (game, server, tools; the game's common environment once as `GAME_ENV`)
and runs nothing, so the command lines can be compared with the implemented flags. One `PASS` or
`FAIL` line per check, a final table, exit 0 only if every selected check passed. Environment:
`MPPM_M53B_TIMEOUT` (seconds per game run, default 1800), `MPPM_GOLDEN` (default strict, passed on),
`MPPM_SERVER_BIN` (default `server/target/release/mppm-server`), `MPPM_DISTROBOX` (default
`mppm-build`).

**Runs.** Every game gets its own home `OUT/<check>/<peer>/home`, its own display, the headless
environment (`SDL_VIDEODRIVER=x11` under `xvfb-run`, dummy audio, no Wayland, no session bus), with
inherited `MPPM_*` online and harness variables removed. Command-line peers get
`MPPM_SKIP_LAUNCHER=1`, `MPPM_WINDOW=640x480`, `--server http://127.0.0.1:47200 --net-bind 127.0.0.1`,
`--checksum-log`, `--record`, `--exit-after-ticks`, `--name` and `--cvar game.idleDemo=false` (a peer
waiting on the title for 43 s would otherwise start the attract movie). The host gets
`--host --ready --slot 0 --code-file OUT/<check>/code --mode M --stage 0`, clients
`--join CODE --slot N --ready` once the code file exists, spectators `--join CODE --spectate` (the host
then gets `--expect` with the member count). Players get `--net-input` of the check's log. Each run
is wrapped as in `desync_ci.sh`: it waits for the display's Xvfb lock to go away, retries once on
`x11 not available`, runs under `timeout`, writes `OUT/<check>/<peer>.exit` and `.end`; peers run in
the background and are stopped by the PID of their `timeout` process (the leader of the game's
process group), `kill -9` goes to the PID of the `mppm` process in that group, never to a
`pkill -f` pattern. Cores newer than the start are deleted.

**Servers.** `server/target/release/mppm-server --bind 127.0.0.1:47200 --relay --relay-bind 127.0.0.1
--relay-port 47201 --relay-ports 47210-47299 --limits-scale 20 --log-file OUT/<check>/server.log`,
one per check (shared by `bat2` and `bat4` with `-P`), stopped with SIGTERM at the end of the check
(so the relay mappings are logged). `turn` adds `build-pc/p2net_turn --bind 127.0.0.1 --port 47202
--user u --pass p` and a second server `--bind 127.0.0.1:47203 --relay-mode static --turn
turn:127.0.0.1:47202 --turn-user u --turn-pass p`.

**Logs.** `netlog.py synth --seed 20261122 --synth-seed 7` with `--vs-stage 0 --ticks 18000` (`B2`),
`--vs-stage 0 --ticks 18000 --players 4` (`B4`), `--ch-stage 0 --ticks 5400` (`C2`) and
`--vs-stage 0 --ticks 3600` (`S2`). WAN profile `W` = `--net-impair delay=46ms,jitter=8ms,seed=<peer index + 1>`.

| Check | Runs | Pass when |
|---|---|---|
| `lines` | - | since `-B`: the only changed file in `src/` or `include/` is `vsGS_Title.cpp` (none before the lockstep package merges), no file added or removed, line counts equal, every added line has `PC_ONLY(`/`PC_SWITCH(`; `tools/net/golden` unchanged |
| `build` | distrobox `ninja -C build-pc mppm p2net_selftest p2transport_test p2net_turn p2lobby_test p2libm_selftest` (the last one for `desync_ci.sh`'s libm check); `cargo build --release --locked` in `server/` | both exit 0 |
| `selftest` | `netlog.py selftest`; `netstats.py --selftest`; `p2net_selftest`; `tests/net/transport/transport_test.sh`; `tests/net/lobby/lobby_test.sh` (both with `MPPM_SERVER_BIN` and `BUILD_DIR` in the environment); `cargo test --locked`; `MPPM_RNG_SELFTEST=1`; `MPPM_NPLAYER_SELFTEST=core\|piki\|vs\|view` | all pass |
| `golden` | `tools/net/m53a_test.sh -d D -o OUT/m53a` (`-q` with `-q`) | exit 0; it includes `desync_ci.sh` with `MPPM_GOLDEN=strict`: offline and harness behaviour unchanged |
| `bat2` (a) | 2P Battle, host + 1 client, `B2`, `W`, 18000 ticks | both exit 0; identical checksum logs; E line at 18000; at least ticks/30 sample lines (C plus L: a sample taken while a Battle round's floor reloads is an L line, and when a round ends depends on the session's random seed), as many as the E line's `lines=`, at least 95% of them C lines; `netlog.py cmp-input` of the two recordings; an offline headless replay of `host.mpnl` with `--checksum-verify host.cksum` exits 0; `netstats.py check --max-stall-rate 2.0 --max-phase-p99-us 2000` on both; more than min(100, C lines / 2) distinct `navi` hashes; `other-in-session=0`; no `PANIC`, `Failed assertion` or `crash screen`; stall rate, d and phase p99 printed |
| `bat4` (b) | 2v2 Battle, host + 3 clients (seats 1-3), `B4`, `W`, 18000 ticks (displays D+2 to D+5) | as `bat2` over 4 logs; every C line has `n=4/`; the recording is version 4 with 4 captains |
| `ch2` (c) | 2P Challenge course 0, `C2`, `W`, 5400 ticks | as `bat2`; the recording is version 5, start mode 2, course 0 (`netlog.py dump`), and its offline replay verifies |
| `relay` (d) | `S2`, both `--net-mode relay`, no impairment, 3600 ticks | both exit 0, identical; `peer ... connected link=relay` in the client log; after the server stops, a `relay mapping` line with up and down bytes above 100 000 |
| `ice` (d) | `S2`, both `--net-mode ice`, 1800 ticks | identical; `link=ice`; `ice connected ... local=host remote=host` |
| `turn` (d) | `S2` on the second server, both `--net-mode ice` with `MPPM_NET_ICE_RELAY_ONLY=1`, 1800 ticks | identical; `ice connected ... relay` in a log |
| `loss` (e) | `S2`, `--net-impair delay=20ms,jitter=4ms,loss=2%,seed=<n>`, 3600 ticks | identical; the replay verifies; stall rate under 5% |
| `drop` (f) | 2v2 `B4` for 4500 ticks, `W`; once the first client's log shows `sync tick=` 1500 or more, `kill -9` of the seat-3 game | the other three exit 0 at 4500 with identical checksum logs; all three print `drop slot=3 tick=E` with the same E; `netlog.py dump host.mpnl --from E-1 --count 5` shows seat 3 neutral; the longest stall stays under 12 s |
| `leave` (f) | `S2`, 2700 ticks, client with `MPPM_NET_TEST_LEAVE_AT=900` | the client exits 0 with `session end reason=left`; the host exits 0 at 2700, prints `drop slot=1 tick=E reason=left` with E within 2 ticks of 900, its longest stall under 1 s |
| `stuck` (f) | `S2`, 2700 ticks, client with `MPPM_NET_TEST_HANG_AT=900:30000` (its game stuck for 30 s, its net thread sending keepalives) | the host exits 0 at 2700 and prints `peer <m> made no progress for 10 s` and `drop slot=1 tick=E reason=timeout` with 900 < E <= 920, its longest stall 9 to 12 s; the client exits 8 with `session end reason=dropped` |
| `desync` (g) | host + client + spectator, `S2`, client `MPPM_NET_TEST_DESYNC=900`, 1800 ticks | all three exit 7 within 30 s of tick 900 (the time the client's `sync tick=` reached 900); each log has `desync at tick 900`; each report file names tick 900 and the session seed (from the checksum log header) |
| `spectator` (h) | `S2` + a spectator, 3600 ticks | all exit 0; the spectator's checksum log equals the host's; the players' stall rates under 2% |
| `ui` (i) | two UI peers (launcher, 1280x960, `MPPM_NET_BIND=127.0.0.1`, a `config.json` with `backend.discPath`, `interface.presetChosen` and `online.serverUrl`) driven by `tools/net/m53b_ui_drive.sh`: settings tab, host, join, ready, start, the client's game stuck for 7 s after tick 450 (`MPPM_NET_TEST_HANG_AT=450:7000`; a `SIGSTOP` would also stop its net thread, and ENet drops a silent peer after 5 s, before the host's Drop button shows), `kill -9` of the host | `ui_settings.png`, `ui_host_room.png`, `ui_join.png`, `ui_client_room.png`, `ui_ready.png`, `ui_waiting.png` (6 s into the hang: the wait pill with its seconds and **Drop Client**), `ui_host_left.png` exist, mean brightness above 3%, every pair differs in more than 2% of pixels (`ui_host_room`/`ui_ready` by region, see below); the logs show `lobby created`, `lobby joined`, `session start`, a stall line and `session end reason=host_left`; files kept in `OUT/ui` |
| `latency` (j) | offline `--latency-probe --vs-stage 0` (640x480), 30 held presses of space (0.1 s down, 0.7 s up) → median M0; online at RTT 0, 30 and 80 ms (`--net-impair delay=0\|15\|40ms,jitter=1ms` on both): the host plays `S2`, the client takes live keyboard input with `--latency-probe`, 30 presses on its display | a table RTT, client d, median, min, max; each median at most `M0 + d x 33.4 + 15` ms; the `bat2`/`bat4` phase error and d printed again |
| `report` | - | the final table: per check PASS/FAIL, stall rate, d, phase p99 and kbps per peer, relay bytes, latency medians (results of earlier runs in the same output directory are shown in parentheses) |

Displays: D and D+1 for two-peer checks, D+2 for a spectator, D+2 to D+5 for `bat4`, D to D+3 for
`drop`. Ports: 47200 (server), 47201 (relay control), 47202 (TURN), 47203 (second server),
47210-47299 (relay ports); the test refuses to start when one of 47200-47203 is in use. Expected time:
about 90 minutes in full, 45 with `-q`, 11 minutes less with `-P`.

Choices of the script where the design leaves room: the distinct-`navi` rule is "more than
min(100, C lines / 2)", so that the quick and Challenge runs (120 and 180 C lines) have a reachable
bar while the full Battle runs keep the design's 100; `ui_join.png` is taken by the driver's `join`
command itself (`M53B_JOIN_SHOT`), after the code is typed and committed and before Join is pressed;
the host room / ready pair of screenshots passes when each region the change touches (the players pane;
the status line, Ready and Start game) differs in more than 2% of its pixels, since only about 1% of
the whole picture changes between those two states of one window; the transport and lobby test scripts get the server binary and the build
directory through the environment (`MPPM_SERVER_BIN`, `BUILD_DIR`); "a stall line" in `ui` is a
`stall` or `summary` line of either peer.

## Results

**Before integration** (package `tests`, branch `online/m53b-tests`, against the base build of
`online/m5.3b` at `aa6b6985e`):

| Test | Result |
|---|---|
| `python3 tools/net/netlog.py selftest` | passed (147 checks, the version 5 ones included) |
| `python3 tools/net/netstats.py --selftest` | passed (38 checks, malformed lines included) |
| `bash -n tools/net/m53b_test.sh`, `-l`, `--dry-run` | clean; the dry run prints every command line and creates nothing but the output directory |
| `tools/net/m53b_test.sh -c lines,golden,report -d 72 -b build-pc` (`online/m5.3b` plus step 0, which changes nothing in the game) | PASS in 1505 s: `lines` (no `src/`/`include/` change yet), `golden` = the full `m53a_test.sh`, all 10 checks, its `desync_ci.sh` matrix, libm and the three golden logs (`MPPM_GOLDEN=strict`) included |
| the same against the base build `<main checkout>/build-pc` (read-only) | every matrix cell and the three golden logs matched; the run counted as FAIL only because that build directory has no `p2libm_selftest` (the libm check); the build check now builds it, and the golden check warns when it is missing |
| plumbing of the session checks | every check from `bat2` to `latency` was run against stand-in binaries (a fake `mppm`, server and `xvfb-run` in a scratch directory) to exercise the script itself: code-file wait, peers by PID, `kill -9` and stop by process group, the server log, the evaluation and the report. Not a test of the game |
| synthetic old formats | `netlog.py synth` of versions 1 to 4 (with and without `--players 4`, events) writes the same bytes as before |

**After integration** (`online/m53b-int`, the full `tools/net/m53b_test.sh -d 72 -o
/var/tmp/mppm-online/m53b -B aa6b6985e`: PASS, exit 0, 4291 s; simulated WAN `W` = 46 ms plus a
uniform 0 to 8 ms jitter each way, a mean RTT of 100 ms; the summary's `rtt_us`, a minimum-RTT
estimate, reads about 93 ms):

| Check | Result | Figures |
|---|---|---|
| lines | PASS | one file changed in `src/`/`include/` (`vsGS_Title.cpp`, the hunk at 192, line count equal, guarded); `tools/net/golden` unchanged |
| build | PASS | `mppm p2net_selftest p2transport_test p2net_turn p2lobby_test p2libm_selftest`; `cargo build --release --locked` |
| selftest | PASS | netlog.py, netstats.py, p2net_selftest, transport_test.sh (relay case against the real server's builtin relay), lobby_test.sh, cargo test (37), RNG and N-player self-tests |
| golden | PASS | full `m53a_test.sh`, 10 checks, `desync_ci.sh` with `MPPM_GOLDEN=strict` (1456 s) |
| bat2 | PASS | 18000 ticks, 600 C lines, replay verifies; stall host 0.01% / client 0.10%; d host 2, client 5; phase p99 276 us; kbps out/in host 65.8/41.2, client 41.6/66.4 |
| bat4 | PASS | 18000 ticks, n=4 on every C line, version 4; stall host 0.01%, clients 0.17/0.21/0.21%; d 2/5/4/5; phase p99 258/265/282 us; host 287.5/117.5 kbps |
| ch2 | PASS | 5400 ticks, version 5, start mode 2, course 0, replay verifies; stall 0.00/0.41%; d 2/4; phase p99 260 us |
| relay | PASS | client `link=relay`; relay mapping up 480153 / down 657627 bytes |
| ice | PASS | `ice connected peer=1 local=host remote=host`, link=ice |
| turn | PASS | `ice connected peer=1 local=relay remote=relay` through `p2net_turn` (static-mode server) |
| loss | PASS | 2% loss, replay verifies; stall host 0.28%, client 2.69% (limit 5%); d 1/3 |
| drop | PASS | seat 3 killed at sync tick 1509; `drop slot=3 tick=1520` on all three; seat 3 neutral from 1520 in host.mpnl; stall 0.47/0.87/1.00% |
| leave | PASS | `drop slot=1 tick=902 reason=left` (left at 900); host stall 0.26% |
| desync | PASS | all three exit 7, 0.7 s after tick 900; reports name tick 900 and the seed |
| spectator | PASS | spectator checksum log = host's; stall host 0.25%, client 0.00% |
| ui | PASS | 7 screenshots in `/var/tmp/mppm-online/m53b/ui` (room, join, client room, ready, waiting overlay, host left, Settings ONLINE) |
| latency | PASS | M0 26.95 ms; RTT 0 / 30 / 80 ms: d 1 / 2 / 4, medians 62.81 / 96.36 / 129.43 ms (limits 75.3 / 108.8 / 175.5) |

**Integration fixes** (details in `M5_3B_LOCKSTEP.md`, deviations table):
- The phase error is no longer wrapped into [-P/2, P/2): after the stage load's long ticks (inside sim
  ticks) the grids were whole ticks apart, the wrap hid it, and the host stalled on every tick.
- `p2_vi_retrace_time_ns` predicts the slewed grid: unslewed, the late-sampling wait slept past an early
  retrace while the one-slot retrace queue held the previous count, so every tick took three retraces.
- Online the tick grid is fixed (a long tick under 4 ticks is caught up, one tick per retrace) and the
  phase controller moves whole ticks at once (`p2_host_tick_shift`, log `phase shift`); the slew only
  corrects the rest. With the slew alone the other players stalled for seconds on a lagging client.
- The connector makes no new attempt once the host link existed (the 15 s timeout fired after a normal
  end); the phase statistics leave out samples taken during a catch-up.
- Test plumbing: the game renames its main thread, so the done test finds the game by its executable;
  the host room and ready screenshots (one window) are compared by region (see "Done test"); the transport test waits
  past ENet's peer timeout under loss; the selftest check starts a server for the transport relay case.

**Fix round 1** (review of the integration):
- Section 10, seat hijack: the host bound a transport peer to the member named by its ENet
  `connectData` (chosen by the remote) and replaced that member's connection before any check, so a
  peer could take a welcomed member's seat without its secret. Now a connection is bound only by a
  HELLO with the member's join secret, never replaces a member already in (a second connection with
  the secret replaces it in the lobby only), and goes after 5 s without one; ICE peers keep the member
  id that signalled the agent. Self-test case `hijack`.
- Section 10, packet size: ENet now refuses packets over 1200 bytes (default 32 MiB) and buffers at
  most 64 KiB per peer; larger received packets are dropped. A member's "addr public" is punched once.
- 7b: a session from the standalone start menu sends the content hash and mod list of the folder's
  enabled mods (read without mounting) instead of the stock ones, and leaves after the boot if the
  mounted set differs.
- `ui_join.png` was shot before the code was typed (1 s into the driver's `join`, typing starts at
  about 2.2 s); the driver now takes it after the code is entered.
- Smaller: host ICE agents of members that leave before connecting, or whose ICE fails, are released;
  offline runs no longer exit 2 over bad values of online flags; `--view-captain` is ignored in a 2-seat
  online session (D16); the capped delay shows a warning toast (D13); `Session::stop` no longer reads the
  transport pointer while the net thread sets it; the end message goes through the endpoint's queue.

After fix round 1, `m53b_test.sh -d 40 -k -c lines,selftest,bat2,bat4,ch2,relay,ice,turn,loss,drop,leave,desync,spectator,ui,latency,report`
passed every selected check (2823 s): bat2 stall 0.04/0.11%, bat4 at most 0.20%, ch2 0.37%, loss
2.28%, drop `drop slot=3 tick=1520` on all three, ICE `local=host remote=host`, TURN `local=host
remote=relay`, the UI flow with `ui_join.png` showing the typed code and Join enabled, latency medians
62.69 / 96.53 / 129.75 ms. `desync_ci.sh --quick` passed.

**Fix round 2** (review of fix round 1):
- Plan 2.8 for a stuck game: the 8 s input timeout counted the net thread's keepalive INPUTs, so a
  seat whose game thread hung, sat on the crash screen or never reached the inject was never dropped,
  and at a load barrier the host could not drop it either. The host now judges progress
  (`HostCore::check_stuck`): a seat its own game has waited for for 10 s mid-match, or one still
  loading 30 s after the host reached a barrier (60 s at the start barrier), is dropped with reason
  `timeout`. The wait overlay offers the host's Drop at a load barrier too (after 10 s) and names the
  players it waits for ("Waiting for Louie to load…"). The crash screen makes the net thread say BYE at
  once, and "Back to start menu" over a match waits up to 300 ms for the BYE before the relaunch. New
  done-test check `stuck` and self-test case `stuck`; the `ui` check now hangs the client's game for 7 s
  (`MPPM_NET_TEST_HANG_AT`) instead of `SIGSTOP` for 4 s, so `ui_waiting.png` shows the Drop button.
- A BYE "done" (`--exit-after-ticks`) that came before the host's own last tick left the host waiting
  for that seat for good; it is now dropped after 1 s of waiting.
- Section 10: a bad or out-of-order CHUNK part goes to the core as the datagram it is (invalid or
  malformed traffic, which ends the sender) instead of being dropped silently; reassembly state is
  erased when its peer disconnects.
- A Hello name (used when the lobby server gave the default "Player") is cleaned like the server's
  `sanitize_name` (valid UTF-8 only, no control characters, trimmed) and may not copy another member's
  name.
- Online recordings are pruned oldest first by the time in their names, not by the random code.
- `Session::code()` (game thread) reads the code under the session mutex the net thread writes it under.
- Server: `--relay-mode` also reads `MPPM_RELAY_MODE` (the commented line in `fly.toml` now names it);
  `docker-compose.yml` passes the TURN secret to `mppm-server` through `MPPM_TURN_SECRET` instead of
  its command line; `server/README.md` has a code map.
- Docs: the D11 row and the 2.10 row describe the time sync as integrated (fixed grid, whole-tick
  shifts, the slew for the rest); "Log lines" states both filters of the phase figures; couch plus
  online is listed as deferred (D17, Deferred 18); M5_1B_HOOKS lists `p2_host_tick_shift`.
- Not changed: the client room's large Ready toggle is the design (SETTINGS 18.3: members get "a large
  **Ready** toggle", `online.rcss` "Members get one big Ready toggle"; changed in fix-min: one row style
  for everyone); the TURN check's one-sided relay
  is a loopback property already listed under "Known limits"; the four identical step-0 commits stay in
  the history (rewriting merged history is not worth it; their content is identical).

After fix round 2, `m53b_test.sh -d 40 -q -c lines,build,selftest,golden,bat2,bat4,ch2,relay,loss,drop,leave,stuck,desync,spectator,ui,report`
passed every selected check (2348 s): bat2 stall 0.11/0.47%, bat4 at most 1.42%, ch2 0.53%, loss
2.42%, drop `drop slot=3 tick=1516` on all three, leave `drop slot=1 tick=902 reason=left`, stuck
`made no progress for 10 s` and `drop slot=1 tick=902 reason=timeout` with a 10.0 s host stall and the
client ending `reason=dropped`, `ui_waiting.png` showing "Waiting for Client (P2)… 6 s" and **Drop
Client**. `desync_ci.sh --quick` passed.

**Fix round 3** (verification of fix round 2):
- The full done test had failed one criterion: a session seed ended a Battle round inside the 18000
  ticks, the floor reload turned one 30-tick sample into an L line, and bat2 had 599 C lines (both
  peers and the offline replay agreed, so the game was right and the rule was not). `bat2`, `bat4`
  and `ch2` now count C and L sample lines: at least ticks/30, as many as the E line's `lines=`, at
  least 95% of them C lines.
- Lobby feedback loop: the host bumped the lobby after every REQ slot, refused ones too, and a client
  with a `--slot` wish repeats its REQ on every LOBBY, so a taken seat (two `--slot 1` joiners, `--slot
  0` while the host sits there, a seat out of range) looped at one round trip per cycle. A refused or
  no-op REQ now changes nothing, and the client repeats a slot REQ at most every 250 ms. Self-test
  case `reqslot`.
- Section 7b: a member reconnecting with its secret had its build and content hashes taken from its
  first Hello; they are now checked again (and its ready cleared when they differ).
- Plan 2.8: after START a lobby `member_left` (`left`/`timeout`) drops the seat only once its P2P
  link has been silent for 1 s, so a stalled WebSocket does not drop a playing member (a kick still
  drops at once; the `drop` check's killed seat went this way, `drop slot=3` 13 ticks after the kill).
- Section 10: well-formed INPUTs are capped at 1000 a second per member (over the cap counts as
  invalid traffic), and the time-sync estimators take at most one sample every 2 ms.
- Server: `joined` always lists the host, also before a REST-created host attached (the game refused
  such a list); a relay REG that raced the session's end no longer leaves a session entry behind (the
  token is looked up again under the relay lock before an entry is made).
- Docs: the Deferred list is numbered again, "Known limits" names the launcher's update check (older
  than M5.3b, `interface.checkForUpdates`; the online code opens no socket offline) and the 1 s grace.
- Not changed: the client room's large Ready toggle is the design (see fix round 2; changed in fix-min); the `ui` check's
  7 s game-thread hang instead of a 4 s `SIGSTOP` is described in the `ui` row (a stopped process
  stops its net thread too and ENet drops it after 5 s, before the Drop button can show).

After fix round 3 the **full** `m53b_test.sh -d 40 -o /var/tmp/m53b-r3/m53b -B aa6b6985e` passed all
19 checks (4451 s), golden included (`m53a_test.sh` full, 1471 s, `desync_ci.sh` with
`MPPM_GOLDEN=strict`): bat2 600 C lines, stall 0.02/0.11%, phase p99 280 us; bat4 stall at most
0.31%; ch2 0.20%; relay, ICE (`local=host remote=host`), TURN (`local=relay remote=relay`); loss
2.33%; drop `drop slot=3 tick=1522` on all three; leave `tick=902 reason=left`; stuck `tick=902
reason=timeout` with a 10.0 s stall; desync, spectator and the UI flow; latency M0 30.37 ms, medians
62.23 / 95.50 / 130.04 ms at RTT 0 / 30 / 80 ms (d 1 / 2 / 3, limits 78.8 / 112.2 / 145.6).
`desync_ci.sh --quick` passed.

**Fix-min** (the minor findings of the final review; details in `M5_3B_LOCKSTEP.md`, "Session flow",
"Desync, drops, spectators, end" and the deviations table):
- Mode switch: 2v2 -> a 2-seat mode with the host in seat 2 or 3 put the host in seat 0 without looking,
  so two members held seat 0 and one became a silent spectator at START. Every member whose seat is
  gone now moves to the lowest free seat or watches (toasts on the host and on that member); the host
  always plays. Self-test `modeswitch`.
- After START a client no longer ends the match on the lobby's `closed host_left` while its P2P link to
  the host is healthy: as on the host, the link decides (1 s of silence). Self-test `hostleft`.
- UTF-8: every wire encoder cuts at a code point boundary; names are 24 code points (up to 96 bytes) as
  the server keeps them; the host's own name (and `resolve_name`) is cleaned and bounded the same way;
  a HELLO name an older encoder cut mid-character is kept without the cut character. Self-test `utf8`.
- Section 10 caps on unverified connections: only admitted, not kicked or banned member ids; at most 2
  per source address (loopback excepted), 2 per member id, 4 in all. Self-test `pendingcap`.
- The member's Ready row is the same settings row as the host's (it was 72dp with a 24dp label).
- Test gaps: the host's Remove before and after START (`kick`) and a spectator-only desync
  (`specdesync`: DESYNC with slot 255, the spectator stops, the players finish) are self-test cases;
  `M5_3B_LOCKSTEP.md` no longer says the done test runs a kick (it does not).
- Found while taking the screenshots below: the in-game Online window showed the lobby's last status
  ("Waiting for the host to start the game.") during the match, now it shows none; a member's own
  content mismatch was stated twice in the room (the warning and the status line), now once; closing
  the wait pill's Drop confirmation reopened an in-game bar the player had closed earlier (a stale
  "was visible" in `MenuBar::uncover`), now the bar stays as it was; a spectator's desync message said
  the match was stopped on every machine, now it says the players go on.

After fix-min, `m53b_test.sh -q -d 40 -o /var/tmp/m53b-fixmin/done` passed all 19 checks (2721 s;
`golden` = `m53a_test.sh -q`, 819 s): bat2 stall 0.08/0.53%, bat4 at most 1.25%, ch2 0.39%, relay
480283/659819 bytes, ICE `local=host remote=host`, TURN `local=relay remote=relay`, loss 2.36%, drop
`drop slot=3 tick=1514` on all three, leave `tick=902 reason=left`, stuck `tick=902 reason=timeout`,
desync 0.6 s after tick 900, spectator, the UI flow, latency medians 63.00 / 96.39 / 129.78 ms at RTT 0 /
30 / 80 ms (M0 27.05 ms). `desync_ci.sh --quick -d 41` passed (50 PASS lines, 382 s); `p2net_selftest`
23 cases and `cargo test` (37) pass.

**UI states checked only against the fake lobby before integration.** The launcher package built and
shot its screens against `m53b_stub_online.cpp` (a scripted fake of `mppm::online`, deleted at
integration). The done test's `ui` check drives only Settings ONLINE, a 2-seat host room, the join pane,
a member room, ready, the wait pill with Drop, and the host-left message, so these states had been seen
only against the fake until fix-min: the 2v2 room with four seats, a content mismatch note, the desync
message, a malformed code, a code nobody uses, the password prompt after a refused join, no lobby server
set, the in-game Online window, the Drop confirmation, the watching line and the toasts of a mode
switch, and the "connection lost" message of a dropped member. Fix-min took headless screenshots of
them against the real `mppm-server` on 127.0.0.1 (two or three games under `xvfb-run`, driven with
`tools/net/m53b_ui_drive.sh` and raw keys; a command-line peer for the third member and for the
password host; a one-file mod for the mismatch):

| State | Screenshot (`/var/tmp/m53b-fixmin/final/`) |
|---|---|
| 2v2 room, four seats (host; member with the same Ready row) | `s1/f_room_2v2_host.png`, `s1/f_room_2v2_client.png` |
| mode switch 2v2 -> Battle (P3 watches, toast) | `s1/f_modeswitch_host.png`, `s1/f_modeswitch_client.png` |
| content mismatch note | `s2/f_content_mismatch.png` |
| desync end message | `s1/f_desync_host.png`, `s1/f_desync_client.png` |
| bad code (malformed; unknown) | `s2/f_bad_code.png`, `s2/f_code_unknown.png` |
| password needed | `s2/f_password_needed.png` |
| no lobby server set | `s2/f_no_server.png`, `s2/f_no_server_join.png` |
| in-game Online window | `s1/f_online_window_client.png`, `s1/f_online_window_host.png` |
| Drop pill, confirmation, after Cancel | `s3/f_waiting.png`, `s3/f_drop_confirm.png`, `s3/f_drop_cancelled.png` |

Still seen only against the fake lobby: the "removed by the host" message (the self-test `kick` covers
the session side) and the Remove button flow in the room. In one run the client's hang fell on the
Battle's "READY?" countdown and the host's wait pill did not show for the whole 8.5 s wait (the session
was waiting; a mid-match hang shows the pill at once, `s3`). Not reproduced and not explained; the
likely place is the hold loop, which draws UI frames only while `aurora_preserve_frame()` keeps the
picture. The host's progress rule still drops a stuck seat after 10 s.

## Deviations from the plan

Decisions D1 to D30 of the M5.3b design where the plan says something else, leaves it open or where
part 1 does less:

| # | Plan | Part 1 | Why |
|---|---|---|---|
| D1 | - | Five packages with disjoint files (server, transport, lockstep, launcher, tests), a shared "step 0" commit (the three headers `transport.hpp`, `lobby_client.hpp`, `p2_online.hpp`, `BOREALIS_ENABLE_WEBSOCKET` on, one optional CMake fragment per package), temporary weak stubs deleted at integration | parallel work without editing each other's files |
| D2 | REST `POST /sessions`, `POST /sessions/{code}/join`, plus WSS signalling | One WebSocket (`GET /v1/ws`) carries create, join, the member list and signalling for the game; the REST endpoints exist too (`/v1/sessions`, `/v1/sessions/{code}/join`, `/v1/health`) and return a token that a WebSocket `attach` redeems | `borealis::http` refuses `http://`, so the game could not reach a local test server over REST; signalling needs the WebSocket anyway. REST stays for tools and other clients |
| D3 | lobby state unspecified | The server knows only membership, relays signalling and enforces kicks; seats, teams, ready, mode, stage, handicaps and the content check live in the host and travel over ENet's reliable channel | one authority (the host) for everything that reaches the simulation; the server stays a thin rendezvous a fork can replace |
| D4 | - | The session package owns the net thread; `Transport` and `LobbyClient` are passive objects it drives | forward-on-complete happens where inputs arrive; ENet is single-threaded |
| D5 | shim `port/net/enet_juice.c` | `port/net/transport/enet_juice.c`; ENet built from the pinned tarball by our own CMake target with `unix.c`'s 8 socket functions renamed by compile definitions; libjuice agents behind synthetic `240.0.0.x` addresses | no ENet source patch |
| D6 | direct first, relay as fallback | `--net-mode auto`: ICE when the server lists ICE servers; else, with the builtin relay offered, direct (LAN, then public with punches) for 1.5 s each, then the relay; else direct. Tests force `direct`, `ice` or `relay` | on one machine every direct path works, so the relay and TURN tests must force the mode |
| D7 | netem for the 100 ms simulated WAN | in-process send-side impairment in the transport, enabled only by `--net-impair` / `MPPM_NET_IMPAIR` | `tc netem` needs root |
| D8, D9 | tag t + d; 8-byte inputs | as planned: a player samples port 0 at read t and tags it t + d_i, with a neutral prefix 1..d_i at the start; `err` from the seat map | - |
| D10 | "one hard jump at the load barrier" | each peer reports `loadedTick`; the host sends `RELEASE(B, G)` once every seated player reached B; the game thread jumps its retrace grid so tick B+1 runs at G on every peer, and drops the stale retrace message of JUTVideo's one-slot queue | holding the bundle on the host does not align anyone |
| D11 | phase-slew time sync: at most 1 ms per retrace, one hard jump at the load barrier | NTP-style offset from the minimum-RTT sample over 2 s; an unwrapped phase error from `(hostTick, hostDue)` in bundles; online the tick grid is fixed (a long tick under 4 ticks is caught up, one tick per retrace); whole ticks of a correction move the grid at once at any time (`p2_host_tick_shift`, log `phase shift`, up to 16 ticks); the slew (at most 1 ms per retrace) corrects only the rest | stalls do not absorb whole-tick differences: after the stage load's long ticks (inside sim ticks) the grids were whole ticks apart, and with the slew alone the others stalled for seconds (M5_3B_LOCKSTEP.md, deviations) |
| D12, D13 | `d_i = max(1, ceil((path_p99 + e + m)/P))` | one-way delays from synced timestamps, `d_i = clamp(ceil((up_i + max other down_j + 3 ms)/P), 1, 8)`, increases at once (the skipped tag is a copy), decreases at barriers, +1 when a seat's blame exceeds 2% over 3 s | RTT/2 hides asymmetric paths |
| D14 | stall loop merged with late sampling | as planned, plus the same hold as a fallback inside `p2_net_exchange`; it also runs with `MPPM_LATE_SAMPLE=0` | a missed wait costs time, never correctness |
| D15 | `p2_mode_pending()/p2_mode_create()` section hook | the session starts at the harness's VS (or Challenge) inject; `START` makes the title leave by itself; the only `src/` edit is `vsGS_Title.cpp:192` (`p2_net_ch_inject`) | the harness path already reaches the stage with the resident archives loaded; the section hook comes with M5.4's modes |
| D16 | each peer renders its own captain | 2 seats keep the stock 2-way split on every peer; 4 seats show the local captain full screen | for 2 captains the stock LOD reads the presented viewports, so a per-peer view would change the simulation; the observer LOD for N = 2 is M5.4 |
| D17, D18 | 2.3 step 1: "one machine may hold several slots (couch plus online)" | one seat per machine: the local player is physical port 0; `--net-input` replays a seat for tests; pre-session reads as in the harness | lobby slot i is logical port i; couch plus online is deferred |
| D19, D20 | `build_hash` + `content_hash` in the handshake | build hash = SHA-256 over the executable, a CMake-generated flags string and shared objects in the executable's directory; content hash as M5.2. A mismatch keeps the member in the room with a note and blocks ready | 7b: "blocks ready and shows what differs" |
| D21 | desync: "report and abort" | the session ends on every peer, each writes a report; the UI restarts into the start menu, command-line peers exit 7 or 8 | an in-process return to the lobby needs a section change with the connection kept (deferred) |
| D22 | spectators, late join | spectators join in the lobby only; a spectator that differs is told and stops, the players go on | late catch-up needs a log transfer and a headless-to-paced switch (deferred) |
| D23 | - | `MPPM_NET_TEST_DESYNC` test hook | the done test's desync case |
| D24 | replays | online sessions always record the confirmed stream; offline replay reproduces the checksum log | the report's input and a strong test |
| D25 | Challenge in Tier 0 | `--ch-stage N` (harness) and `.mpnl` version 5 (start mode 2, course, handicaps, online flag) | Challenge determinism is checked offline before the online test |
| D26 | `online.server_url` | key `online.serverUrl` (camelCase as every key, alias `online.server_url`), plus `online.playerName`; the default server is empty until the official one is deployed | SETTINGS.md key style; no contact with a domain that is not run yet |
| D27 | HTTPS/WSS only | `ws://` only to loopback or with `--net-allow-plaintext` | local tests need plaintext; production stays TLS |
| D28 | - | an unfocused window sends neutral input; a minimized window still blocks its game thread and stalls the others | known limit, deferred |
| D29 | - | `--net-test-bots N`: in-process bots on an in-memory transport | the session package could test the game side before the transport and server merged |
| D30 | - | exit codes 0, 2, 7, 8, 9 | the done test needs them |

Plan items of the M5.3b row that part 1 does **not** meet (see [Deferred](#deferred)): the
Cloudflare TURN spike and the Fly deployment, matrix A and the 30-minute sessions, a session through a
self-hosted compose stack with coturn, the relayed share of real peers, own-captain prediction (2.10
item 5) and "checksums identical with prediction on and off", the latency options (item 6, allowed to
come later), and the latency figures on a probe rig under netem (part 1 measures press-to-present in
software under Xvfb, which is not a display latency).

## Known limits

- Every test ran on one machine over loopback. ICE on loopback only proves the client path (host
  candidates; relay candidates through libjuice's own TURN server); real NAT behaviour is untested.
- The impairment is simulated in the sender; it does not model bandwidth limits, bursts of loss or
  path changes.
- Latency under Xvfb is press-to-present in software, not press-to-photon.
- 2P online shows the stock split on every peer; a full-screen own view for 2 seats needs the
  observer LOD for 2 captains (M5.4).
- A minimized window stalls everyone (D28); an unfocused one sends neutral input.
- After a desync or a host loss the session ends; there is no return to the lobby, no rejoin and no
  host migration.
- Spectators can only join in the lobby.
- The default server URL is empty until the official server exists; the UI asks for one.
- Only Linux builds have the transport (the shim uses `unix.c`; Windows and macOS later).
- A start-menu session announces the folder's mod set; if the mount then drops a mod (a disc-dependent
  check), that peer leaves the session after the boot rather than desync. Mod mismatches are tested in
  the self-test (`lobby`) and were seen end to end once in fix-min's screenshots, not in the done test.
- Unverified connections are capped (2 per address, 2 per member id, 4 in all, admitted ids only), but
  loopback is not capped per address, and an admitted member can still keep its own two places busy
  for 5 s at a time; ENet's own half-open handshakes (before `Connected`) are not counted.
- A crashed peer whose lobby WebSocket the kernel closes is reported as `reason=left`, like a leave.
  After START the host takes a lobby `member_left` (`left` or `timeout`) as final only once that
  member's P2P link has been silent for 1 s (a stalled WebSocket alone does not drop a playing seat; a
  kick drops it at once), so a killed game is dropped about 1 s after its last packet. A client treats
  the lobby's `closed host_left` after START the same way (fix-min): the match ends only once the
  host's P2P link has been silent for 1 s.
- Offline, the online code opens no socket. The launcher's update check (a setting older than M5.3b,
  `interface.checkForUpdates`, on by default and asked on first run) does connect to GitHub when the
  launcher starts; with it off, or with `MPPM_SKIP_LAUNCHER`, an offline boot opens no TCP or UDP socket.
- `MPPM_NET_ICE_RELAY_ONLY=1` removes non-relay candidates from signalling only; which pair ICE picks
  varies (relay/relay, or host/relay with a peer-reflexive candidate on the other side), so the `turn`
  check proves a TURN relay on at least one side, not both.

## Deferred

Part 2 of M5.3b, or the user (accounts and deployments):

1. **The Cloudflare TURN spike** (plan 2.11 "Early check"): a libjuice agent against
   `turn.cloudflare.com` carrying ENet traffic; needs a Cloudflare account and API token. Part 1
   proved the same client path against libjuice's own TURN server on 127.0.0.1.
2. **The Fly.io deployment**: done 2026-11-25 (`moonsprout-lobby.fly.dev`, relay mode `none` with
   Cloudflare STUN, scale to zero, `MPPM_DEFAULT_SERVER_URL` set). Still open: a domain, the deploy on tag,
   nightly SQLite backups to R2, and `relay=cloudflare` credential minting.
3. **Real two-machine tests**: matrix A, real NAT and WAN, 30-minute Tier 0 sessions and a 30-minute
   4-peer 2v2, one session through Cloudflare TURN and one through a self-hosted `docker compose`
   stack (coturn), the relayed share of peers, a minimized or unfocused window on a real desktop, a
   pad on another port before the session.
4. Press-to-photon latency with the rig on every peer under real netem; real-display latency runs.
5. Prediction of the own captain, cursor and whistle with local cues and muted sim sounds (2.10 item
   5), and checksums identical with prediction on and off.
6. Latency options (2.10 item 6): Immediate present, X11 compositor bypass, one DMA buffer, sticky
   edges.
7. Late spectators (bundle log transfer and headless catch-up), the spectator camera picker, free cam.
8. Rejoin at boundaries, host migration, the hybrid mesh fast path (2.10 item 7) with equivocation
   checks.
9. The host-election hint (all-pairs and relay pings before ready).
10. The equal-delay option; a drop vote after a stall rate above 5% for 10 s (part 1: the host's drop
    button and the automatic drop on disconnect).
11. Run-length coding of the redundancy block and other bandwidth work (part 1 measures kbps).
12. An in-process return to the lobby after a match or a desync; Rematch and Rematch swap sides;
    results, leaderboards, replay upload.
13. Ticking while minimized (D28).
14. A full-screen own-captain view for 2 seats (the observer LOD for 2 captains; changes 2-captain
    checksums).
15. Mod-pack download for mismatched peers.
16. Windows and macOS builds of the transport (Winsock in the shim, `win32.c` renames) and cross-OS
    play.
17. Quick-chat, emotes, pings; kick and ban UI beyond the host's Remove.
18. Several seats on one machine (couch plus online, plan 2.3 step 1 and 3.7): part 1 has one seat
    per machine, physical port 0 (D17).
19. A nightly 30-minute 4-peer soak and a latency regression job in CI.
20. A `wss://` test against a real certificate (part 1 tests `ws://` on loopback only).
