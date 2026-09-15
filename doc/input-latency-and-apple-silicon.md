# Barrier: input latency reduction + Apple Silicon build support

## Context

Two separate asks, answered from an audit of the tree at `653e4bad`.

**Latency.** The original idea was to move Barrier's transport from WiFi to Bluetooth. That would make latency worse, not better: BLE has a 7.5 ms minimum connection interval and 15–30 ms typical one-way latency with jitter into the hundreds of milliseconds, Bluetooth Classic RFCOMM runs 10–40 ms with retransmit stalls, while 5 GHz WiFi to the same AP is 1–3 ms and wired Ethernet is 0.1–0.3 ms. Bluetooth also has no IP stack, so it would need an entirely new `IArchNetwork` backend, and its ~1 Mbit/s practical ceiling would make clipboard sync and drag-and-drop file transfer crawl. The transport is not where the latency is.

The real per-event costs are in the stack above the socket, and an audit located four of them precisely (details in each task below). Notably, `TCP_NODELAY` is _already_ correctly applied — `TCPSocket::init()` (`src/lib/net/TCPSocket.cpp:305-328`) is called from both constructors, before `connect()`, and `SecureSocket` inherits it — so the usual first guess is already done. There is also no server-side throttling or coalescing of mouse moves, and the `SocketMultiplexer` poll timeout is `-1` (infinite, `src/lib/net/SocketMultiplexer.cpp:189`), so neither of those is the problem either.

**Apple Silicon.** The tree already compiles and runs native arm64 — but only if you bypass the build scripts entirely and hand-write the cmake flags. The scripts themselves are broken on any Apple Silicon machine without full Xcode.app installed, and carry an Intel-era deployment target that is below the arm64 floor. The goal here is to make the documented build path work, not to produce a universal binary or a signed distributable.

**Intended outcome.** Measurably lower input latency on the existing TCP transport with no protocol change; an optional UDP input channel for lossy networks; and `./clean_build.sh` working on an Apple Silicon Mac with Command Line Tools and Homebrew.

## Scope decisions

- **No Bluetooth transport.** Rejected on the latency and bandwidth grounds above.
- **Latency: ship the safe wins now**, without an instrumentation-first phase. Instrumentation is still included as an optional verification aid in Phase 1, because the improvement is otherwise unmeasurable (see the note on log timestamp resolution in Verification).
- **UDP input transport: yes, but opt-in and phased separately.** Flagging the tension explicitly: this is _not_ a safe win. It is a protocol change requiring a new `ClientProxy1_7`, a minor-version bump, new Arch-layer datagram support on two platforms, and a real answer to the question of keystrokes on an unauthenticated channel. It lands after Phase 1 and behind a default-off flag.
- **Apple Silicon: build scripts + native arm64 only.** No universal binary, no codesigning, no notarization, no CI changes.

---

## Phase 0 — Apple Silicon build support (do this first: it unblocks building at all)

Goal: `./clean_build.sh` succeeds on Apple Silicon with Command Line Tools only and Homebrew at `/opt/homebrew`. Native arm64, no universal binary.

Independent of the latency work. Nothing else can be built or measured until this lands.

### 0.0 Prior art upstream — checked, not reusable

`origin/enhancement/builds/macos-universal` attempts exactly this area (reworks `clean_build.sh`, renames `osx_environment.sh` to `macos_environment.sh`, touches `azure-pipelines.yml`). Do not branch from it:

- It **abandons the universal build** at commit `5d50eec1 "Disable arm64 & x86_64 Mac builds - Qt causes build failures"`. Homebrew's `qt@5` keg is thin (single-arch), so a universal Barrier cannot link it. This independently confirms the native-arm64-only scope chosen above — universal is blocked by the dependency, not by our build files.
- Its `clean_build.sh` still hardcodes the Xcode.app-only SDK path and `-DCMAKE_OSX_DEPLOYMENT_TARGET=10.9`, so it does not fix 0.1 or 0.2.
- It introduces two shell bugs: `continue` used outside a loop in the cmake-detection block, and `$B_CMAKE "$B_CMAKE_FLAGS" ..` which passes every flag as a single argv entry.

Worth mirroring one thing from it: renaming `osx_environment.sh` to `macos_environment.sh` matches upstream's direction and reduces future merge conflict. Optional.

### 0.1 Accept Command Line Tools instead of requiring Xcode.app

`osx_environment.sh:15` calls `check_dir_exists '/Applications/Xcode.app' 'Xcode'`, whose helper (`:4-12`) prints `Please install $package` and `exit 1`. Because `clean_build.sh:16` sources this file with `.` rather than running it as a subprocess, that `exit 1` kills the entire build script.

Replace the directory check with a capability check: verify `xcode-select -p` succeeds and `xcrun --show-sdk-path` returns an existing directory. Both work under Command Line Tools. Keep a clear error message naming both install options (`xcode-select --install` or full Xcode).

### 0.2 Resolve the SDK with `xcrun`, and raise the deployment target

`clean_build.sh:17` builds the sysroot path by hand:

```sh
-DCMAKE_OSX_SYSROOT=$(xcode-select --print-path)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
```

That layout exists only inside Xcode.app. Under CLT the path is `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`. Use `xcrun --show-sdk-path`, which is correct under both.

The same line sets `-DCMAKE_OSX_DEPLOYMENT_TARGET=10.9`. arm64 macOS starts at 11.0, and clang does not clamp — `-arch arm64 -mmacosx-version-min=10.9` silently yields `__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ 1090`, so the arm64 slice is built against a nonsense minimum and Homebrew's much newer Qt 5.15 produces "built for newer macOS version than being linked" diagnostics. Raise to 11.0 on arm64. 10.9 also already contradicts `dist/macos/bundle/Barrier.app/Contents/Info.plist.in:33-34`, which declares `LSMinimumSystemVersion` 10.12.0 — reconcile these rather than leaving three different answers in the tree.

### 0.3 Point the Homebrew branch at the actual Homebrew prefix

`osx_environment.sh:30-40` correctly uses `brew --prefix qt@5` for Qt (`:32`) but then hardcodes `/opt/procursus` into every search path:

```sh
export CMAKE_PREFIX_PATH="/opt/procursus:$QT_PATH:$CMAKE_PREFIX_PATH"
export LD_LIBRARY_PATH="/opt/procursus/lib:$LD_LIBRARY_PATH"
export CPATH="/opt/procursus/include:$CPATH"
export PKG_CONFIG_PATH="/opt/procursus/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Nothing ever adds `/opt/homebrew/{include,lib,lib/pkgconfig}`. This was invisible on Intel because `/usr/local/{include,lib}` are in clang's default search path; `/opt/homebrew` is not. Derive these from `$(brew --prefix)` and drop the unconditional `/opt/procursus` prefix (keep it only if that directory exists).

Separately, `LD_LIBRARY_PATH` (`:38`) is inert on macOS — the dyld variable is `DYLD_LIBRARY_PATH`. It has presumably never done anything here. Fix or remove it rather than leaving a line that reads as if it works.

### 0.4 Give the Darwin OpenSSL chain an else branch, and fix its ordering

`CMakeLists.txt:323-367` hardcodes a four-way search — `/opt/procursus`, `/opt/local`, `/usr/local/opt/openssl`, `/opt/homebrew/opt/openssl` — and never calls `find_package(OpenSSL)`, so `-DOPENSSL_ROOT_DIR` is ignored. Two defects:

- **No `else` branch.** The chain's `endif()` at `:367` closes it with no fallback and no `message(FATAL_ERROR)`, unlike the generic branch at `:371-380` which Darwin never reaches. If only a versioned keg exists (`openssl@3` with no unversioned `openssl` symlink) configure succeeds silently with `OPENSSL_LIBS` unset, and the failure surfaces much later as undefined symbols at link. Add a `FATAL_ERROR` else branch naming the expected paths. This is dormant on the current host only because the symlink happens to exist.
- **Wrong order for Apple Silicon.** `/usr/local/opt/openssl` (Intel Homebrew, `:347`) is tested _before_ `/opt/homebrew/opt/openssl` (`:357`). On a machine with both prefixes present, an arm64 build picks the x86_64 OpenSSL. Test the Apple Silicon prefix first when `CMAKE_SYSTEM_PROCESSOR` is `arm64`, or better, derive the prefix from `brew --prefix openssl` and fall back to the hardcoded list.

Also accept a versioned keg (`openssl@3`, `openssl@1.1`) so the unversioned-symlink requirement stops being load-bearing.

### 0.5 Unblock CMake 4.x

`CMakeLists.txt:18` and `src/gui/CMakeLists.txt:1` both declare `cmake_minimum_required (VERSION 3.4)`. CMake 4.x removed `<3.5` compatibility and hard-errors with _"Compatibility with CMake < 3.5 has been removed from CMake."_ The installed CMake here is 4.4.3. Raise both to a supported floor. Use the range form (`VERSION 3.5...3.27`) so newer policies are opted into deliberately rather than by accident.

This removes the need for the `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` workaround that a manual configure currently needs.

### 0.6 Stop the `--sysroot` splice from eating the next flag

`CMakeLists.txt:141-142`:

```cmake
if (APPLE)
    set (CMAKE_CXX_FLAGS "--sysroot ${CMAKE_OSX_SYSROOT} ${CMAKE_CXX_FLAGS} -DGTEST_USE_OWN_TR1_TUPLE=1")
```

If `CMAKE_OSX_SYSROOT` is empty, clang consumes `-DGTEST_USE_OWN_TR1_TUPLE=1` as the sysroot argument. The bundled googletest is 1.6.0 (2011), whose auto-detection at `ext/gtest/include/gtest/internal/gtest-port.h:443-460` then falls through to `#include <tr1/tuple>`, which does not exist in libc++ — producing the confusing `fatal error: 'tr1/tuple' file not found`.

The `--sysroot` here is redundant in the first place: CMake already emits `-isysroot` from `CMAKE_OSX_SYSROOT`. Drop it and keep only the define. If it must stay, guard it on the variable being non-empty.

### 0.7 Update the build documentation

`CLAUDE.md` on the `docs/claude-md` branch documents the manual cmake invocation as the workaround for all of the above, and `README.md` carries the macOS build instructions. Once 0.1–0.6 land, replace the manual invocation with the now-working `./clean_build.sh` path and keep a short note on what the flags used to be for. Leave the Accessibility-permission note — that is a runtime concern and still accurate.

### Explicitly out of scope for this phase

Called out so they are a decision, not an oversight. All were found in the audit and all are real:

- **No codesigning, entitlements, notarization, or hardened runtime exist anywhere in the repo.** `dist/macos/bundle/reref_dylibs.sh:48` runs `install_name_tool -change` and never re-signs, which invalidates any signature. On Apple Silicon every Mach-O needs at least an ad-hoc signature to execute. This matters for distributing a `.app`, not for a locally built binary.
- **`CFBundleIdentifier` is the bare string `barrier`** (`Info.plist.in:12-13`), not reverse-DNS. Combined with no stable signing identity, macOS TCC grants for Accessibility are fragile and re-prompt across rebuilds — so an Accessibility grant can be invalidated by a rebuild.
- **Nothing checks Input Monitoring.** `AXIsProcessTrusted()` is checked (`src/lib/platform/OSXScreen.mm:116-127`, `src/gui/src/main.cpp:163-190`) but macOS 10.15+ requires _separate_ Input Monitoring consent for `CGEventTapCreate(kCGHIDEventTap, …)` at `OSXScreen.mm:755-757`. Without it the tap is created non-NULL but delivers no events, and the only handling is `LOG((CLOG_ERR "failed to create quartz event tap"))` at `:779`. `IOHIDCheckAccess`/`IOHIDRequestAccess` is the missing call. This is a genuine "silently does nothing" trap and is worth a follow-up task.
- **No universal binary.** `CMAKE_OSX_ARCHITECTURES` is set nowhere in the tree.
- **CI is entirely x86_64** on three retired Azure images (`azure-pipelines.yml:85-97`: macOS-11, 10.15, 10.14), and `:104` does `rm -rf /usr/local/opt/openssl`, an Intel-prefix assumption. CI also invokes `clean_build.sh`, so it depends on exactly the chain fixed above.
- **`Screensaver detection is dead on modern macOS.`** `src/lib/platform/OSXScreenSaver.cpp:169-199` uses 32-bit-era Carbon Process Manager APIs (`GetNextProcess`, `GetProcessInformation`, `CopyProcessName`); they compile but return nothing useful.

---

## Phase 1 — Safe latency wins on the existing TCP transport

No protocol change, no new files, so no `cmake -S . -B build` re-run is needed. Ordered safest first; each is a separate commit so a bisect lands cleanly.

### Corrections to the initial audit

Two of the four candidate targets were mis-framed in the first pass. Both corrections are verified against the macOS SDK and the tree, and both change what the work is worth:

- **The macOS suppression interval is not a remote-latency fix.** `CGSetLocalEventsSuppressionInterval` is `API_DEPRECATED("No longer supported", macos(10.0,10.6))` (`CGRemoteOperation.h:376-380`), i.e. the existing calls in `enter()`/`leave()` are almost certainly no-ops on both sides today. More importantly the SDK defines the semantics as "the period of time in seconds that **local hardware events may be suppressed** after posting a Quartz event" (`CGEventSource.h:230-234`, default 0.25 s) — it throttles the _client's own_ keyboard and trackpad, not the injected event. Zeroing it will not move a latency number. It fixes a real but different bug: the client's local input is swallowed for 250 ms after remote input.
- **The X11 25 ms poll does not delay arriving input.** `XWindowsEventQueueBuffer::addEvent` (`:238-272`) writes to a self-pipe that is `pfds[1]` in the same poll, under the same mutex that sets `m_waiting`, so a delivered event wakes the poll immediately. `TIMEOUT_DELAY` only quantises _timer_ deadlines (`remaining` is decremented by the literal 25 regardless of actual sleep) and costs idle wakeups. Deferred — see 1.5.

One further constraint that shapes everything below: **TLS is on by default** (`ArgParser.cpp:286-290`; only `--disable-crypto` turns it off). Since `SecureSocket` must opt out of the inline-write optimization, task 1.4 does nothing for the default configuration until its phase 2.

### 1.1 One message, one write (`PacketStreamFilter::write`)

`src/lib/barrier/PacketStreamFilter.cpp:93-106` issues two `getStream()->write()` calls — 4-byte length, then payload. The first is what sets `wasEmpty` and triggers `setJob`, so the multiplexer can win the race and emit a 4-byte segment followed by an 8-byte one. Under TLS it is two `SSL_write`s, i.e. two TLS records with ~29 bytes of overhead each.

Build header+payload into one buffer and issue a single write. **Use a fixed stack buffer with fallback**, not an unconditional copy: `PROTOCOL_MAX_MESSAGE_LENGTH` is 4 MiB (`protocol_types.h:58`) and file/clipboard chunks are 32 KiB (`StreamChunker.cpp:38`), so an unconditional merge means a 32 KiB heap copy per chunk. `UInt8 packet[512]` covers every latency-critical message (mouse move is 12 bytes on the wire, keys ~14); anything larger keeps today's two writes, since large transfers are throughput-bound, not latency-bound.

Wire format is byte-identical, so a patched peer interoperates with an unpatched one. Risk: very low — an off-by-one in the threshold or memcpy offsets would corrupt every message, which `integtests` catches immediately.

### 1.2 macOS event source, done with the live API

Today every injected event is created with a `NULL` source (`OSXScreen.mm:478`, `:564`, and the scroll event at `:674`) — verified. Per-source settings only apply to events created _with_ that source, so no suppression configuration can currently take effect no matter what is called.

- Create one `CGEventSourceRef` in the `OSXScreen` constructor via `CGEventSourceCreate(kCGEventSourceStateHIDSystemState)` (the state `OSXDragSimulator.mm:89` already uses), store it as a member near `m_eventTapPort` (`OSXScreen.h:326`), `CFRelease` in the destructor, tolerate `NULL` by falling back to today's behaviour.
- Configure it once: `CGEventSourceSetLocalEventsSuppressionInterval(src, 0.0)` plus the two `CGEventSourceSetLocalEventsFilterDuringSuppressionState` calls matching `avoidSupression()`'s intent. Note the spelling — the modern constants are `kCGEventSuppressionState*` (double-p); the single-p names in the current code are compatibility `#define`s at `CGRemoteOperation.h:390-399`.
- **Pass the source instead of `NULL`** at `:478`, `:564`, `:674`. This is the load-bearing part.
- `postMouseEvent`/`fakeMouseWheel` are `const`; create the source in the constructor rather than lazily so no `mutable` is needed.
- Fix the unbalanced pragma while in the file: `:2118` should be `push` + `ignored`, `:2158` should be `pop`. Currently harmless only because `:2158` is the last line of the file — it becomes a trap the moment anything is appended.

Choose `kCGEventSourceStateHIDSystemState` over `kCGEventSourceStatePrivate`: a private source carries independent modifier state and is more likely to break sticky-modifier handling. The code already overrides flags (`:485`, `:570`, `:681`) and click state (`:481`, `:566`) explicitly.

Risk: low-moderate. Switching from `NULL` changes what state created events inherit. Regression to watch: sticky or dropped modifiers on the client.

**Expectation management — say this in the PR, not after:** this will not move a latency number. It is worth shipping as "the client no longer eats your own trackpad input for 250 ms after remote input," plus removing dead deprecated API.

### 1.3 Release the post-warp suppression on the macOS server

This is the macOS change with actual latency upside, and it sits next door to 1.2. `OSXScreen::onMouseMove` calls `warpCursor()` → `CGWarpMouseCursorPosition` (`OSXScreen.mm:1102`, `:266-278`) on **every** motion event while the primary drives a remote screen, inside the synchronous `CGEventTap` callback. Warp suppression is governed by the same interval whose setters are dead, so the live control is `CGAssociateMouseAndMouseCursorPosition(true)` immediately after the warp. The same call already appears at `:708` and `:735` with a "fixes mouse randomly not showing" comment, so it is known-good in this process.

Separate commit from 1.2 so it can be reverted independently. Risk: low code risk, **unknown cost** — this is a WindowServer IPC on every motion event at up to 1000 Hz. It must be measured, not assumed. If server CPU climbs noticeably during sustained motion, revert it.

### 1.4 Opportunistic inline write (`TCPSocket::write`)

`TCPSocket::write` (`src/lib/net/TCPSocket.cpp:158-188`) never calls `send()`. It buffers, then wakes the multiplexer via a self-pipe byte, which rebuilds the entire pfd vector and calls `poll` a second time before `doWrite` runs. Verified prerequisite: **all `IStream::write` traffic is on the event-queue thread** — `ProtocolUtil::vwritef` makes exactly one `stream->write()` per message, and the file/clipboard chunkers `addEvent()` rather than writing from their worker thread, so the multiplexer never calls `TCPSocket::write`. Sockets are already non-blocking (`ArchNetworkBSD.cpp:115`, `:247`), so an inline `send()` cannot stall the event loop.

Design:

- Add `protected: virtual UInt32 tryWriteInline(const void*, UInt32);` to `TCPSocket` — returns bytes written, 0 if not attempted; `m_mutex` must be held. Body is `ARCH->writeSocket` wrapped in `catch (XArchNetwork&) { return 0; }`.
- **`SecureSocket` overrides it to `return 0;`. This is mandatory** — `SecureSocket` does not override `write()`, so an unguarded raw inline write in the base class would put plaintext on a TLS socket, and would corrupt the `do_write_retry_` invariants (`SecureSocket.cpp:226-252`).
- Restructure `write()`: keep the `!m_writable` and `n == 0` guards verbatim; if `wasEmpty && m_connected && m_socket != NULL`, try inline; on a full write emit `outputFlushed` and return with **no `setJob` at all** — that is where the wakeup and second poll disappear; otherwise buffer the remainder at `buffer + wrote`, set `m_flushed = false`, release the lock, and `setJob` as today.

Locking, to be restated in the commit message: the inline write introduces **no new lock** and must happen **while holding `m_mutex`** — that is exactly what serialises it against a concurrent `doWrite()` on the multiplexer thread and guarantees byte ordering. `setJob` stays outside the lock; moving it inside creates the classic inversion against `SocketMultiplexer::addSocket`.

Error handling is deliberately **not** duplicated: any `XArchNetwork` from the inline attempt is swallowed and the data buffered, so the multiplexer hits the same error and produces exactly the events `serviceConnected` produces today. No second copy of the shutdown logic to drift. `outputFlushed` is still emitted even though it has no handlers anywhere in the tree, so the event stream stays bit-identical.

Risk: moderate — the only change touching the concurrency model, which is why it lands last. Regressions to watch: message reordering if the inline path ever runs with a non-empty buffer (guarded by `wasEmpty` plus the held mutex); partial-write accounting (buffering `buffer` instead of `buffer + wrote` silently duplicates bytes — `integtests` file transfer catches it); TLS plaintext leak if the `SecureSocket` override is ever dropped.

**Phase 2, optional and later:** override `tryWriteInline` in `SecureSocket` to bail on `!isSecureReady() || do_write_retry_` and otherwise call `secureWrite()`, treating `WANT_WRITE` as "wrote nothing, buffer it". This is the only way 1.4 helps the default TLS configuration. Ship it only after phase 1 has soaked — retry-state bugs here manifest as hung connections, not crashes.

### 1.5 X11 poll cadence — deferred, not dropped

Per the correction above, this buys nothing on the input path. If taken later: pass `min(remaining, TIMEOUT_DELAY)` as the poll timeout and decrement `remaining` by _measured_ elapsed time rather than the constant, leaving the 25 ms ceiling so idle CPU is unchanged. Lowering the constant itself raises idle CPU linearly for no input-latency benefit. Recommendation: do not ship with this batch — zero measurable benefit on macOS, non-zero blast radius on every X11 build.

---

## Phase 2 — Optional UDP input transport (protocol 1.7, default off)

Only after Phase 1 is landed and measured. This is the large, risky piece; everything here is behind `--udp-input`, default off.

### 2.0 The finding that makes this tractable

**No `IArchNetwork` changes are required** — verified directly: `kDGRAM` is already declared (`src/lib/arch/IArchNetwork.h:72`) and mapped to `SOCK_DGRAM` in both backends (`src/lib/arch/unix/ArchNetworkBSD.cpp:59`, `src/lib/arch/win32/ArchNetworkWinsock.cpp:33`), and `newSocket()` is type-agnostic. `connect()` a UDP socket to the peer (which never blocks and does no handshake) and `bindSocket`/`connectSocket`/`pollSocket`/`readSocket`/`writeSocket` all work unmodified while preserving datagram boundaries. The kernel then drops datagrams whose source doesn't match the peer — free off-path injection protection.

Four consequences that shape the design:

1. `TCPSocket::init()` (`src/lib/net/TCPSocket.cpp:305-328`) calls `setNoDelayOnSocket`, i.e. `setsockopt(IPPROTO_TCP, TCP_NODELAY)`, which returns `ENOPROTOOPT` on a UDP fd and throws `XSocketCreate`. **The UDP socket must not derive from `TCPSocket`.**
2. `readSocket()` returning 0 is ambiguous on UDP (zero-length datagram vs. `EAGAIN`), and `TCPSocket::doRead` (`:358-369`) treats 0 as EOF and fires `inputShutdown`. A separate multiplexer job is mandatory.
3. BSD `read()` silently truncates an oversized datagram; Winsock `recv()` returns `WSAEMSGSIZE` → `XArchNetwork`. Use a 2048-byte read buffer and catch per-datagram `XArchNetwork`.
4. ICMP port-unreachable surfaces as `ECONNREFUSED`/`WSAECONNRESET` on the next read or write; the socket stays usable, so catch-and-ignore is correct. Count these toward the fallback heuristic rather than adding the `SIO_UDP_CONNRESET` ioctl (which would need a new `IArchNetwork` method).

### 2.1 New files

| File                                                                                            | Purpose                                                                                                                                                                                                                                             |
| ----------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/lib/barrier/MemoryStream.{h,cpp}`                                                          | `IStream` over a `std::vector<UInt8>` + read cursor. The keystone: lets `ProtocolUtil::writef`/`readf` marshal into a datagram payload with **zero changes to `ProtocolUtil.cpp`**, so the UDP encoding of `DMMV` is byte-identical to the TCP one. |
| `src/lib/net/UdpSocket.{h,cpp}`                                                                 | `: public ISocket` — deliberately **not** `IDataSocket`, **not** `IStream`, **not** `TCPSocket`. Datagram API plus one permanent read-only `TSocketMultiplexerMethodJob`, so no new thread.                                                         |
| `src/lib/net/UdpCrypto.{h,cpp}`                                                                 | AES-256-GCM seal/open (`EVP_aes_256_gcm`) + 64-bit sliding anti-replay window. Protocol-ignorant, so it unit-tests cleanly.                                                                                                                         |
| `src/lib/barrier/UdpChannel.{h,cpp}`                                                            | Shared state machine `Idle → Offered → Probing → Active → Down`. Owns the socket, per-direction keys, tx sequence, rx replay window, screen generation, liveness timer, fallback decision.                                                          |
| `src/lib/server/ClientProxy1_7.{h,cpp}`                                                         | `: public ClientProxy1_6`. Overrides the eight input senders; owns the server-side `UdpChannel`.                                                                                                                                                    |
| `src/test/unittests/net/UdpCryptoTests.cpp`, `src/test/unittests/barrier/MemoryStreamTests.cpp` | See verification.                                                                                                                                                                                                                                   |

`file(GLOB)` means new files need a `cmake -S . -B build` re-run before building, or they are silently absent from the binary.

### 2.2 Files modified

`protocol_types.{h,cpp}` (version 6→7, new message codes); `ClientProxyUnknown.cpp` (add `case 7:` to the switch at `:207-235` — verified it currently covers 0–6 with no default); `ServerProxy.{h,cpp}` (new parse branches + a `handleUdpInput` that feeds decrypted payloads through the **existing** `parseMessage`, reusing `ServerProxy::mouseMove` at `:705` including its mouse-move compression); `SecureSocket.{h,cpp}` (add `exportKeyingMaterial` via `SSL_export_keying_material`, **taking `ssl_mutex_`** and gated on `m_secureReady` — both confirmed present); `Client.cpp`; `ArgsBase.{h,cpp}` and `ArgParser.cpp` (`--udp-input`, `--udp-port`); `Server.cpp` (one change: set/bump the screen generation on enter/leave).

`parseMessage`'s handlers hardcode `m_stream`. Refactor them to take an `IStream*` parameter rather than adding a `m_currentStream` member — the member is a smaller diff but a shared-mutable-state footgun.

**Explicitly untouched**, and this is what guarantees the TCP path cannot regress: `ISocketFactory.h`, `TCPSocketFactory`, `IDataSocket.h`, `TCPSocket.cpp`, `SocketMultiplexer`, `PacketStreamFilter.cpp`, `ProtocolUtil.cpp`, `IArchNetwork.h`, and both Arch network backends.

### 2.3 Wire format

New TCP control messages: `kMsgDUdpOffer = "DUDO%2i%4i%4i"` (port, session id, flags), `kMsgDUdpAccept = "DUDA%2i%4i%4i"`, `kMsgDUdpDecline = "DUDD%1i"`, `kMsgCUdpUp = "CUDU"`, `kMsgCUdpDown = "CUDD%1i"`, `kMsgDKeySync = "DKSY%4i%2I%1i"`. `%2I` already reads a `std::vector<UInt16>*`, so no marshalling extension is needed.

Inside datagrams: the eight existing input messages unchanged, plus `kMsgDUdpPing = "DUPG%4i"` / `kMsgDUdpPong = "DUPO%4i"`.

Datagram layout — 70 bytes, constant:

```
off  len  field
  0    4  magic 'B','U','D','P'
  4    1  version (=1)
  5    1  flags (bit0: 1=control/probe, 0=input)
  6    2  screen generation
  8    4  session id
 12    8  sequence number, big-endian (also the AEAD nonce counter)
         -- bytes 0..19 = AEAD associated data: authenticated, not encrypted --
 20   34  ciphertext of the 34-byte padded plaintext
 54   16  GCM authentication tag
```

Plaintext is **always padded to a constant 34 bytes** (2-byte real length, the message, zero padding). This matters: unpadded, a passive observer distinguishes a 12-byte mouse move from a 14-byte keystroke, which is an inter-keystroke-timing oracle for password inference. Padding costs ~20 bytes/packet and removes the size channel outright. Max datagram 548 bytes is below the IPv4 576-byte minimum reassembly, so fragmentation is impossible. One message per datagram — no batching in v1, so every datagram is independently droppable.

### 2.4 Security

This section is not optional and the risk is not symmetric. Plaintext UDP carrying keystrokes gives a same-network attacker a keylogger, and — far worse — an **injection channel**: anyone able to put a UDP packet on the client's port synthesizes keystrokes into the victim's session, which is remote code execution in all but name.

- **Fail closed.** UDP input is offered only when the TCP control channel is TLS. `ArgsBase::m_enableCrypto` already defaults to `true` (`ArgsBase.cpp:45`), so this costs typical users nothing. `--udp-input` with `--disable-crypto` logs a warning and never sends the offer. **Do not add an insecure escape hatch** — it would be copy-pasted out of forum posts by people who do not understand what they are enabling.
- **Every datagram is AEAD-sealed** with AES-256-GCM via OpenSSL (already linked; no new dependency). Keys come from `SSL_export_keying_material(ssl, buf, 64, "EXPORTER-barrier-udp-input-v1", …)` — **no key material ever crosses the wire**. Separate key per direction, so a reflected datagram cannot be replayed into the opposite direction. Nonce = 4-byte salt ‖ 8-byte sequence; uniqueness follows from the monotone counter, and a new TLS session yields new keys, so cross-reconnect nonce reuse is impossible. Tear down on renegotiation or counter exhaustion rather than risk reuse.
- **Associated data is the full 20-byte header**, so an on-path attacker cannot rewrite the sequence number to defeat replay protection or the generation to retarget a datagram at the wrong screen.
- **Anti-replay**: the standard IPsec/DTLS 64-bit sliding bitmap. Plus one protocol-aware rule — a late `kMsgDMouseMove` is **dropped** (a stale absolute position visibly yanks the cursor backwards), while a late key/button event is **applied**, because key events are not idempotent and delivering late beats not delivering.
- **The client never trusts an attacker-supplied address.** `kMsgDUdpOffer` carries only a port; the IP comes from the peer address of the already-established TLS connection. A forged or MITM'd offer cannot redirect the keystream to a third host.
- **Residual risk, stated plainly**: packet _timing_ is still observable, and inter-keystroke intervals are a published password-inference channel. Constant-size padding removes the message-type channel; only constant-rate transmission removes the timing channel, which is not worth the cost here. This is **not a regression** — the current TCP path sets `TCP_NODELAY` and writes once per event (`ClientProxy1_0.cpp:352`, `PacketStreamFilter.cpp:94`), leaking the same timing today. Document it; do not claim UDP fixes it.

### 2.5 Negotiation, and the enter/leave ordering hazard

Negotiation begins **only after the 1.x handshake completes and `forClientProxy().ready()` fires** — a bug in UDP setup must never be able to prevent a connection. Sequence: server binds, exports keying material, sends `DUDO` → client validates preconditions, binds, sets peer from the TCP peer IP, sends `DUDA`, starts sending `DUPG` every 200 ms → server answers `DUPO` → client, having now proven **both** directions (the receiver is the party that can confirm reachability), sends `CUDU` → server goes Active.

**The ordering hazard is the highest-risk part of this design.** `CINN`/`COUT` stay on TCP while input moves to UDP, and the two have no mutual ordering. A UDP keystroke arriving _after_ the TCP leave types into the screen the user already left — a password into the wrong window.

Fix: **screen generation tagging** in header bytes 6–7. The server sets it from the enter `seqNum`, which `kMsgCEnter` already carries as `$3` (`protocol_types.cpp:25`) — so both sides derive it from an existing message and no new ack is needed. The client drops any input datagram whose generation does not match the enter it is inside, and drops all input while not entered. Because the generation is inside the AEAD associated data it cannot be forged. The server bumps it on every leave, rejecting in-flight datagrams.

Two things ship alongside it: `Screen::leaveSecondary()` already calls `fakeAllKeysUp()` (`src/lib/barrier/Screen.cpp:562`), bounding stuck-key damage to one screen visit — keep it. And **the first mouse move after each enter goes over TCP**, with UDP taking over from the second, to avoid a visible cursor stall at the start of each visit.

### 2.6 Loss handling

`kMsgDMouseMove` is absolute, so a lost move self-heals on the next one ~8–16 ms later — which is why it migrates first. `kMsgDKeyUp`/`kMsgDMouseUp` do **not** self-heal; a lost key-up leaves a modifier stuck. Three layers:

1. **Duplicate every key-up and mouse-up** ~30 ms later with a fresh sequence number. At 2 % loss this takes stuck-key probability from 2 % to 0.04 %. Because the retransmission uses a new sequence, the transport delivers it twice — application-level idempotence is required, so verify `KeyState::fakeKeyUp` is a no-op for an already-released key before relying on this.
2. **`kMsgDKeySync` over TCP** — the authoritative backstop, sent when the held set empties, every 250 ms while anything is held, and on leave. The client releases keys absent from the list and **never synthesizes a key-down from a resync**: auto-repeating a key the user already released is strictly worse than a briefly stuck modifier.
3. **`fakeAllKeysUp()` on leave**, which already exists.

### 2.7 Fallback

| Trigger                         | Action                                                                  |
| ------------------------------- | ----------------------------------------------------------------------- |
| Preconditions unmet             | Never offer; behaviour byte-identical to today                          |
| No `DUDA` within 2 s            | Abandon silently                                                        |
| `DUDA` but no `CUDU` within 5 s | The NAT/firewall case — abandon, log `CLOG_NOTE`, cost to user is zero  |
| 3 consecutive 1 s ping timeouts | `CUDD`, revert to TCP; one re-offer after 60 s then give up (anti-flap) |
| >8 AEAD open failures in 10 s   | `CUDD`                                                                  |
| Socket send error               | `CUDD` immediately                                                      |

Invariant: **at every instant either UDP is Active or input goes over TCP — there is never a state where input is dropped.** The switch is a single boolean read on the main thread, so no race with the multiplexer.

### 2.8 Sub-phasing

Each independently shippable and revertable. **0**: `MemoryStream`, `UdpSocket`, `UdpCrypto`, unit tests — nothing wired in. **1**: version bump + `ClientProxy1_7` as a pure pass-through, UDP still disabled; run the full compatibility matrix here where the diff is one `case` label. **2**: negotiation only, channel carries pings only — verifies fallback and teardown without risking a real input event. **3**: mouse move over UDP; measure here, this is where the feature proves itself or does not. **4**: keys and buttons, plus `DKSY` and key-up duplication. **5**: `--udp-stats`, docs, GUI checkbox.

---

## Verification

### Build first

There is no `build/` directory in the tree, so step zero is a working build. Until Phase 0 lands, configure by hand with `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, `-DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)"` and `-DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"`; after it, `./clean_build.sh` should work directly. Submodules are already checked out (gtest/gmock 1.6.0, gulrak-filesystem).

### Automated regression net

- `./build/bin/integtests` — `NetworkTests` drives real loopback TCP through `ClientListener` → `PacketStreamFilter` → `TCPSocket` for both mock data and mock file transfer. This is the primary guard for 1.1 and 1.4; run before and after each. It uses `TCPSocketFactory` (plaintext), so it does exercise the inline path.
- `./build/bin/unittests` — baseline is **133 passed, 2 failed** (`SecureUtilsTest.FormatSslFingerprintHexWithSeparators`, `SecureUtilsTest.CreateFingerprintRandomArt`). These fail on a clean checkout. Do not chase them.

### Single-machine smoke test

```sh
cp doc/barrier.conf.example /tmp/barrier-test.conf
build/bin/barriers -f -d DEBUG -n moe -c /tmp/barrier-test.conf --address :24800   # terminal A
build/bin/barrierc -f -d DEBUG -n larry 127.0.0.1:24800                            # terminal B
```

Two things this plan depends on getting right:

- **Add `--disable-crypto` to both ends when testing 1.4.** TLS is the default and `SecureSocket` opts out of the inline write, so without the flag you are testing the unchanged path. Run it both ways — with crypto to prove the opt-out did not break the TLS handshake or transfer, without to prove the inline path works.
- **Do not switch screens in a single-machine run.** The client injects via `CGEventPost` into the same WindowServer the server's tap is reading; once `m_isOnScreen` goes false, `onMouseMove` warps to centre and re-sends (`OSXScreen.mm:1096-1113`), producing a runaway feedback loop. Single-machine verifies connect, handshake, keep-alive, clipboard and clean disconnect only. **Real latency work needs a second machine** (or a VM as client).

### Measuring — the log timestamp cannot show this

`Log::print` formats with `time()`/`localtime` at 1-second resolution (`src/lib/base/Log.cpp:171-176`), so log timestamps can never show a sub-millisecond change. Three methods instead:

**1. Segment count — proves 1.1, needs zero code.**

```sh
sudo tcpdump -i lo0 -tt -n -S 'tcp port 24800' | head -200        # single machine
sudo tcpdump -i en0 -tt -n -S 'tcp port 24800'                    # two machines
```

Move the mouse on the remote screen. Before: alternating 4-byte and 8-byte payloads. After: a single 12-byte payload per motion. Binary pass/fail, not a statistic.

**2. Enqueue-to-wire latency — proves 1.4. Temporary instrumentation, never committed.** `Stopwatch` is backed by `ARCH->time()` = `gettimeofday`, microsecond resolution (`src/lib/arch/unix/ArchTimeUnix.cpp:47-52`). On a scratch branch, record `ARCH->time()` in `write()` when `wasEmpty`, log the delta in `discardWrittenData` (`TCPSocket.cpp:460-469`) when the buffer hits zero, and log the same on the inline success path. Capture 30 s of motion at `-d DEBUG2` before and after, compare mean and p95. Measured entirely within one process, so there is no clock-skew question. Expect the baseline to show the self-pipe wakeup plus pfd rebuild plus second poll as tens-to-hundreds of microseconds against single-digit microseconds inline. **Delete before the PR** — a per-motion DEBUG2 log is itself a latency source.

**3. End-to-end, two machines — the only way to evaluate 1.3.** Log `ARCH->time()` at `%.6f` at `ClientProxy1_0.cpp:352` (server) and `OSXScreen.mm:619-641` (client) and diff. Treat the absolute number as unreliable (NTP skew) and the before/after _difference_ as the result. For 1.3 specifically, compare both the latency distribution and server CPU (`top -pid $(pgrep barriers)`) during sustained motion with and without the `CGAssociateMouseAndMouseCursorPosition` call — if CPU climbs noticeably, revert.

**4. Functional check for 1.2.** A one-shot `CGEventSourceGetLocalEventsSuppressionInterval(m_eventSource)` log after creating the source confirms the setter took (expect `0.000000`). Then, on the client, physically move its own trackpad while the server drives it — before the change local motion is dropped in ~250 ms bursts, after it composes with remote motion.

### UDP-specific (Phase 2)

Simulated loss with `dnctl` + `pfctl` (dummynet), degrading **only** the UDP port so the TCP control channel stays pristine — degrading both conflates the transports:

```sh
sudo dnctl pipe 1 config plr 0.10 delay 30
printf 'dummynet out proto udp from any to any port 24801 pipe 1\ndummynet in  proto udp from any to any port 24801 pipe 1\n' > /tmp/barrier-loss.conf
sudo pfctl -f /tmp/barrier-loss.conf -e
# ...test...
sudo pfctl -d && sudo dnctl -q flush
```

This is why `--udp-port` should be pulled forward from sub-phase 5 — matching a fixed port is far easier than chasing an ephemeral one. Remove any `set skip on lo0` for a loopback run. If SIP restricts `pfctl`, fall back to Network Link Conditioner (less precise; it hits TCP too).

Compatibility matrix, every sub-phase from 1 onward: 1.6 client ↔ 1.7 server (confirm via `tcpdump` that **no `DUDO` is ever written**); 1.7↔1.7 both flags off; server on / client off (decline path); both on with `--disable-crypto` (must refuse and log); both on with TLS (active path); UDP port `pfctl`-blocked (fallback path); loss at 0/2/10/30 %. **The single highest-value manual test: block the UDP port mid-session while holding a modifier and typing** — exercises teardown and the key-up resync simultaneously.

If p99 latency does not improve materially at 2 % loss, the UDP feature is not earning its complexity and should be dropped rather than shipped.

## Release notes

`doc/newsfragments/`, per the repo convention of `<slug>.<type>`:

- `lower-input-latency.feature` — each protocol message is now sent as a single TCP segment, and small writes bypass the socket multiplexer wakeup.
- `osx-client-event-suppression.bugfix` — macOS: local keyboard and mouse input on the client is no longer suppressed after injected remote input (the old Quartz suppression API has been unsupported since macOS 10.6). Fold 1.3 in here if it ships.
- `macos-apple-silicon-build.bugfix` — the build scripts now work on Apple Silicon with Command Line Tools and Homebrew at `/opt/homebrew`.
- Phase 2 only: `udp-input-transport.feature`, and **in bold** the one-directional compatibility break — a 2.5 client cannot connect to a 2.4 or earlier server.

No fragment for the pragma fix; not user-visible.

## Top risks

1. **Phase 2 enter/leave ordering.** If generation tagging is wrong, keystrokes land on the screen the user already left. Worst realistic outcome is a password typed into the wrong window. Test by switching screens rapidly while typing under loss.
2. **Phase 2 version bump breaks new-client ↔ old-server**, verified: `ClientProxyUnknown.cpp:243-246` throws `XIncompatibleClient` and replies `kMsgEIncompatible` when no case matches. Unavoidable — adding messages without bumping is worse, since `ClientProxy1_0.cpp:160` calls `disconnect()` on any unrecognized code. One-directional: a 1.7 server still accepts 1.0–1.6 clients. Optional mitigation worth considering: have the client retry the handshake at 1.6 on receiving `EICV`, which would eliminate the break entirely.
3. **1.4 concurrency.** The only change touching the threading model. Lands last and alone so a bisect points straight at it.
4. **Forgetting the `SecureSocket` opt-out in 1.4** puts plaintext on a TLS socket. Guard with a test, and note it in the header comment on `tryWriteInline`.
5. **1.3 may cost more than it saves** — an unmeasured WindowServer IPC per motion event. Separate commit, revert if CPU climbs.
6. **Expectation mismatch on 1.2.** It does not reduce latency. If it ships described as a latency fix, the result will read as a failure.
