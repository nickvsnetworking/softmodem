# softmodem

A **virtual modem over SIP**. `softmodem` presents an ordinary serial port with a
Hayes/`AT` command set to any software, while carrying the modem signal as **G.711
(PCMA/PCMU) RTP over a SIP call**. The data modem itself is **Bell 212A / V.22 at
1200 bps** (spandsp), which is what Nortel Millennium payphones use.

The original goal: run [`mm_manager`](https://github.com/hharte/mm_manager) (the Nortel
Millennium payphone Manager) against a payphone wired to an **ATA**, over SIP, with **no
physical USB modem**. But nothing here is payphone-specific — to the host it is just a
serial modem, so it works with any modem software.

```
  mm_manager (or any modem app)            softmodem                         ATA + payphone
  ─────────────────────────       ───────────────────────────             ────────────────
   open()/AT/data                  AT engine + call FSM
   /dev/ttyMODEM0  <───PTY───>     │  +  Bell 212A soft modem  <──G.711 RTP──>  FXS ── phone
   (looks like a real serial port) │  +  SIP (sofia) / RTP (oRTP)     SIP
```

## How it works

- A **PTY** is exposed (symlinked to e.g. `/dev/ttyMODEM0`); the app opens it like any
  serial port, sends `AT` commands, and sees `RING` / `CONNECT 1200` / `NO CARRIER`.
- Inbound SIP `INVITE` → `RING` → (auto-)answer → `200 OK`. Outbound `ATD` → `INVITE`.
- Audio is **G.711 8 kHz, 20 ms frames**. spandsp's `v22bis` modem modulates/demodulates
  the 1200 bps data; an internal 8N1 UART framer keeps the idle line clean.
- One sofia `su` reactor thread runs SIP **and** a 20 ms timer that pumps RTP↔modem; the
  main thread owns the PTY and AT engine.

## Why no echo cancellation

Bell 212A / V.22 is full-duplex by **frequency-division** (originate = low band, answer =
high band); each receiver's band filter rejects its own near-end hybrid echo. A voice
(G.168) echo canceller would *corrupt* the modem tones. So `softmodem` does **not** cancel
echo — and the ATA / PBX / PSTN path **must** have **EC, VAD, comfort-noise and AGC
disabled** too, or training will fail.

## Build

Dependencies (Debian/Ubuntu):

```sh
sudo apt-get install -y libspandsp-dev libsofia-sip-ua-dev libortp-dev cmake build-essential
cmake -B build -S . -DWITH_SIP=ON
cmake --build build
```

Build tiers (each enables the layer above): default (PTY + AT only), `-DWITH_DSP=ON`
(adds the spandsp modem), `-DWITH_RTP=ON`, `-DWITH_SIP=ON` (full stack; implies the
others).

## Usage

```
softmodem -b <ip:port> -I <media-ip> -P <media-port> -d <tty> [options]
```

Direct peer-to-peer to an ATA at `192.168.1.50` (answers inbound, dials it on `ATD`):

```sh
softmodem -b 192.168.1.10:5060 -I 192.168.1.10 -P 40000 -p 192.168.1.50 -d /dev/ttyMODEM0
```

Register to a PBX:

```sh
softmodem -m registrar -r pbx.local -u softmodem -w secret \
          -b 192.168.1.10:5060 -I 192.168.1.10 -P 40000 -d /dev/ttyMODEM0
```

Then point your software at the serial device, e.g.:

```sh
mm_manager -f /dev/ttyMODEM0
```

`mm_manager`'s default init string (`ATE=1 S0=1 S7=3 &D2 +MS=B212`) is handled as-is;
`S0=1` makes softmodem auto-answer the inbound SIP call.

> **Bind a specific interface IP, not `0.0.0.0`.** sofia-sip mishandles a wildcard bind on
> multi-homed hosts (`Address already in use`). Use the host's real LAN IP.

> The default tty path `/dev/ttyMODEM0` needs write access to `/dev` (run as root or pick a
> writable path with `-d`, e.g. `-d /tmp/ttyMODEM0`). If the symlink can't be created,
> softmodem prints the underlying `/dev/pts/N` to use instead.

### Options

| Flag | Meaning |
|------|---------|
| `-d, --device` | PTY symlink path (default `/dev/ttyMODEM0`) |
| `-b, --bind` | SIP bind `ip:port` (use a real IP, not 0.0.0.0) |
| `-I, --media-ip` / `-P, --media-port` | Local RTP IP / base port advertised in SDP |
| `-m, --mode` | `direct` (default) or `registrar` |
| `-p, --peer` | ATA / peer `ip[:port]` for direct mode |
| `-r/-u/-w` | registrar host / SIP user / password |
| `-c, --config` | INI config file (same keys; CLI overrides) |
| `-v` | more logging (repeatable) |

## ATA configuration (important)

For the modem to train over the ATA's FXS port:

- Codec **G.711 (PCMA or PCMU) only**.
- **Disable** echo cancellation, VAD/silence-suppression, comfort noise, and any voice AGC.
- Enable **modem / data passthrough** if available; use a **small fixed** (non-adaptive)
  jitter buffer.
- Keep the ATA and softmodem on the **same LAN** with no transcoding hops — 1200 bps DPSK
  tolerates almost no packet loss or jitter.

## AT commands

Supported: `ATZ`, `ATE`, `ATQ`, `ATV`, `ATS<r>=<v>` / `ATS<r>?` (incl. `S0` auto-answer),
`AT&F`/`&D`/`&C`, `AT+MS=...` (accepted), `ATA`, `ATD<target>`, `ATH`, `ATO`, `ATI`, `+++`
escape. Result codes: `OK`, `ERROR`, `RING`, `CONNECT 1200`, `NO CARRIER`.

`ATD` targets: a bare number becomes `sip:<number>@<peer>`; or pass a full `user@host`.

## Self-tests (no hardware)

```sh
./build/softmodem --selftest   # spandsp v22bis loopback through G.711: train + byte round-trip + idle quiet
./build/softmodem --rtptest    # oRTP paced send/recv integrity over localhost UDP
```

Two-instance SIP loopback (one answers, one dials) over localhost validates the full
SIP + RTP + modem path end-to-end.

## Status / limitations

- Line rate is **1200 bps Bell 212A / V.22** (matches the Millennium). No V.32/V.34/V.90.
- Registrar mode performs `REGISTER` with digest auth; the direct-IP path is the most
  exercised.
- Against a **real payphone** you may need to enable the Bell answer tone
  (`ANSWER_TONE_MS` in `src/engine.c`) and tune the ATA — see comments in the source.
