# VLESS Protocol Evolution Log

This document records the protocol and runtime changes made while evolving this project toward fuller VLESS compatibility.

## Scope of this round

- Align request-header parsing closer to Xray-core VLESS semantics.
- Keep the current coroutine + io_uring architecture, but make it safer for multi-thread workers.
- Add baseline UDP data-plane support with VLESS length-packet framing.
- Avoid "false support": explicitly reject commands that are parsed but not fully implemented yet.

## Framework/Base Runtime Changes

### Multi-thread coroutine safety

- `CoroutineRegistry::instance()` changed from process-global singleton to `thread_local`.
- Result: each worker thread owns an isolated coroutine resume table, matching the "one io_uring per thread" model.

### Event loop wiring with validator context

- `EventLoop` now takes a `vless::Validator` reference in constructor.
- Every accepted `VlessConnection` receives the same validator reference for UUID auth.

### Multi-worker bootstrap

- Main process now creates N `EventLoop` workers (`N = hardware_concurrency` by default, overridable by argv).
- Multi-worker bind uses `SO_REUSEPORT`.

### Why this direction (with reference to `cpp-http-server`)

- `cpp-http-server` base uses coroutine awaitables + io_uring completion-driven resume.
- This project keeps the same design style, but with per-thread coroutine registry isolation so each thread's CQE resumes only its own coroutines.
- No cross-thread task scheduling is introduced.

## VLESS Protocol Changes

### UUID validation

- Removed hardcoded single UUID check in decoder.
- Added `proxy::vless::Validator`:
  - supports UUID parsing from canonical string form;
  - supports multi-user lookup;
  - deduplicates users.
- Runtime config source:
  - env `VLESS_USERS=uuid1,uuid2,...`;
  - fallback to legacy default UUID if env is empty.

### Command parsing

- `Command` enum now includes:
  - `TCP = 1`
  - `UDP = 2`
  - `Mux = 3`
  - `Rvs = 4`
- Decoder now handles `Rvs` and maps to destination `v1.rvs.cool` like Xray naming conventions.

### Addons parsing

- Header addons parser now follows protobuf varint flow, not fixed 1-byte assumptions.
- Parsed fields:
  - `field 1` -> `flow`
  - `field 2` -> `seed` (bytes)
- Unknown/unsupported wire types now fail fast instead of silent partial parse.

### Address handling

- Decoder read paths now check exact-length reads and throw explicit parse errors on short reads.
- IPv6 textual conversion in logs is now real `inet_ntop` output instead of placeholder.

### Target connect family support

- Outbound target creation in `VlessConnection` now supports:
  - IPv4 direct connect
  - IPv6 direct connect
  - domain resolution with `AF_UNSPEC` and first supported A/AAAA result
- Connect syscall now uses actual sockaddr length (`socklen_t`) instead of fixed `sockaddr_in` size.

### UDP support (new)

- `Command::UDP` now enters a real UDP relay path instead of being rejected.
- Target socket uses `SOCK_DGRAM` and `connect()` to selected destination.
- Client payload format follows VLESS UDP body convention:
  - uplink: `[len(2 bytes, big-endian)] + payload`
  - downlink: same framing back to client.
- Handshake buffered remainder is consumed as first UDP framed payload bytes.
- Current constraint: UDP is only enabled when `flow` and `encryption` are both empty.
  - UDP + Vision/Encryption is still rejected explicitly for now.

### Explicit non-support behavior (important)

- `Mux` / `Rvs` are still explicitly rejected at connection stage.
- Rationale:
  - parser recognizes them (protocol-awareness),
  - relay semantics are not yet implemented,
  - explicit reject is safer than silently treating them as TCP.

## Compatibility impact summary

- Better:
  - multi-user UUID compatibility;
  - safer header parsing on fragmented or malformed input;
  - IPv6 destination compatibility;
  - baseline UDP forwarding compatibility.
- Intentionally strict:
  - non-TCP commands now fail fast instead of producing undefined relay behavior.

## Remaining work for "more complete protocol"

1. Implement UDP + Vision/Encryption compatibility policy and framing rules.
2. Implement real Mux channel semantics (not only command parse).
3. Implement Rvs mode handshake and bridge semantics.
4. Decide final policy for custom encryption extension (currently parser no longer relies on nonstandard addons field).
5. Add interop tests against Xray client/server matrices (TCP, UDP, mux, vision, IPv6).
