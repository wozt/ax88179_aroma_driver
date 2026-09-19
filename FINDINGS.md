# AX88179 Wii U Aroma - Current Findings


[✓] IPv4 / ARP
[✓] DHCP
[✓] ICMP
[✓] TCP client
[✓] UDP
[✓] select
[✓] nonblocking
[✓] errno / SO_ERROR
[✓] socket option translation principale
[✓] SNDBUF / RCVBUF ABI mesurée
[✓] IP_MULTICAST_TTL
[✓] IP_MULTICAST_LOOP
[✓] IP_MULTICAST_IF
[✓] IP_ADD_MEMBERSHIP
[✓] IP_DROP_MEMBERSHIP
[✓] réception multicast réelle
[✓] émission multicast réelle

[✓] limites négatives exactes SNDBUF/RCVBUF
[ ] comportement réel SNDBUF / backpressure
[✓] TCP listen / accept réel
[✓] sendto_multi_ex
[✓] recvfrom_multi
[✓] recvfrom_ex avec vrai TTL
[✓] sendto_multi
[ ] gethostbyaddr
[ ] DNS async / variantes restantes
[ ] NSSL : état/options pendant promotion
[ ] hot-unplug / reconnect
[ ] perte/restauration link
[ ] DHCP renew/recovery
[✓] exhaustion sockets
[ ] exhaustion buffers
[ ] charge/concurrence

## Mario Maker / Pretendo validation — NSSL bridge and SO_TCPSACK (2026-09-18)

Mario Maker was used as a real-title compatibility test with:

```ini
[debug]
shim_trace=0

[compat]
dns=system
route=ax
```

Two independent compatibility problems were found.

### NSSL cannot use an AX/lwIP-backed descriptor directly

The shim exposes a real nsysnet placeholder fd to the title while the actual AX connection lives in lwIP. Mario Maker passed that public fd to `NSSLCreateConnection()`.

Before the bridge was added, `NSSLCreateConnection()` returned a connection handle, but `NSSLDoHandshake()` failed with `NSSL_ERROR_IO_ERROR` because NSSL was operating on the unconnected native placeholder rather than the connected lwIP socket.

The compatibility bridge now intercepts `NSSLCreateConnection()`. For an AX-owned socket it:

1. obtains the connected peer using `lwip_getpeername()`;
2. connects the reserved native placeholder fd to the same peer;
3. removes the public fd from AX/lwIP ownership;
4. closes the lwIP socket;
5. calls the real `NSSLCreateConnection()` with the connected native fd.

This is intentionally a hybrid path. Ordinary title TCP/UDP sockets can use the AX88179, but a connection handed to NSSL is promoted to the native Wii U network stack at the TLS boundary.

After this change, Mario Maker's Pretendo discovery request completed normally:

```text
GET /v1/endpoint
Host: discovery.olv.pretendo.cc

HTTP/1.1 200 OK
Content-Length: 300
```

The returned discovery XML contained the expected Pretendo `api.olv.pretendo.cc` endpoints.

### Wii U socket-option numbers are not interchangeable with lwIP

After discovery, Mario Maker created another TCP socket but closed it before calling `connect()`.

Deferred tracing isolated the sequence:

```text
SO_SNDBUF  -> success
SO_RCVBUF  -> success
SO_TCPSACK -> failure
socketclose()
```

On Wii U, `SOL_SOCKET / 0x0200` is `SO_TCPSACK`.

In lwIP, the same numeric value `0x0200` represents `SO_REUSEPORT`, and lwIP does not expose the Wii U per-socket SACK switch through its socket API. Passing the Wii U option number directly to `lwip_setsockopt()` therefore failed.

The shim now defines:

```c
#define NSN_SO_TCPSACK 0x0200
```

and handles it explicitly as a compatibility no-op rather than forwarding the numeric value to lwIP.

SACK is a TCP optimization rather than a requirement for TCP correctness, so this reproduces the title-visible success behavior without accidentally enabling an unrelated lwIP option.

After this fix, the socket continued normally through:

```text
socket
setsockopt
non-blocking connect
NSSL promotion
TLS handshake
HTTPS request
HTTPS response
```

### Validation result

With both the NSSL bridge and the `SO_TCPSACK` compatibility fix in place, Mario Maker successfully:

* opened Course World through Pretendo;
* downloaded a course;
* launched the downloaded course;
* played the downloaded course normally.

This validates the current hybrid `route=ax`, `dns=system` design for the tested Mario Maker / Pretendo workflow.

### General compatibility rule

Do not assume that nsysnet socket-option values are numerically compatible with lwIP.

Socket options must be translated or explicitly emulated case by case. `SO_TCPSACK = 0x0200` demonstrated that the exact same numeric value can represent a completely different option in lwIP.

Also avoid logging directly from title socket or NSSL hooks. Earlier testing showed that logging from timing-sensitive hooks can alter networking behavior or interfere with per-thread socket error state. Production hooks should remain silent; diagnostic information should be collected in memory and emitted later from another thread when debugging is required.


## Major validation — Minecraft + Pretendo through AX88179 (2026-09-18)

The GAME-process `nsysnet` shim has now been validated with a real commercial Wii U title.

**Minecraft: Wii U Edition successfully reached gameplay and created a map while its game sockets were routed through the AX88179/lwIP stack, with Pretendo active.**

Validated runtime configuration:

```ini
[dhcp]
mode=keep_first

[debug]
shim_trace=0

[compat]
dns=system
route=ax
```

In this configuration:

* IPv4 game sockets created through the intercepted `nsysnet` API are routed through lwIP and the AX88179 USB Ethernet adapter.
* DNS remains on the native Wii U networking stack so Inkay/Pretendo DNS handling continues to work.
* FTP port `21` and wiiload port `4299` remain native by design.
* This is therefore currently a hybrid setup: supported game sockets use AX/lwIP, while selected Wii U system networking remains native.

### lwIP UDP netbuf pool bug

During the Minecraft/Pretendo NAT-check sequence, raw Ethernet tracing showed that multiple UDP replies physically reached the AX88179, but initially only the first two reached the application socket.

The cause was lwIP's default netbuf pool size:

```c
MEMP_NUM_NETBUF = 2
```

Each queued UDP datagram consumes one `struct netbuf`. The stack could therefore retain only two datagrams from the NNCS burst even though the UDP receive mailbox itself was larger.

The current configuration is:

```c
#define MEMP_NUM_NETBUF 32
#define DEFAULT_UDP_RECVMBOX_SIZE 32
```

After this change, Minecraft received the complete observed NNCS burst instead of only the first two packets.

### Pretendo NNCS observations

Minecraft performs an NNCS/NAT-check sequence using UDP port `59941`.

Observed requests included:

* type `101` to NNCS1 port `10025`;
* type `102` to NNCS1 port `10025`;
* type `103` to NNCS2 port `10025`;
* additional traffic to ports `33334` and `33335`.

On both the custom AX path and the native Wii U network path:

* five type `101` replies were received;
* no type `102` reply reached the console;
* five type `103` replies were received.

Because the same missing type `102` behavior occurs on the known-working native Wii U network path, the absence of the type `102` reply is **not the Minecraft failure condition on this network**.

After the NNCS phase, Minecraft also sends three UDP datagrams to NNCS1 port `33335`.

Pretendo intentionally treats port `33335` as a sinkhole and does not reply, so the lack of a response there is expected.

### Debug logging can break NEX

A major debugging trap was discovered while investigating Minecraft.

With verbose socket tracing enabled, Minecraft failed during its NEX networking sequence even when `route=native` forced the game sockets through the original Wii U networking stack.

Once native comparison mode was made silent, Minecraft worked normally again.

The same test was then repeated with:

```ini
route=ax
shim_trace=0
```

and Minecraft successfully entered gameplay and created a map through the AX88179 path.

This established that the final Minecraft failure was caused by the debugging instrumentation rather than by the actual AX network transport.

NEX frequently performs a non-blocking socket operation and then immediately queries `socketlasterr()`. Logging from inside or immediately after a socket hook can itself perform networking and alter timing or last-error state.

Rules established from this result:

* Do not call `WHBLogPrintf` directly from timing-sensitive socket paths when validating games.
* Do not insert logging between a failed native socket operation and a later `socketlasterr()` call.
* Packet-level NNCS/NEX traces should not be enabled during normal gameplay.
* Use `shim_trace=0` for normal game compatibility.
* Prefer RAM counters, deferred traces, or worker-thread logging for future debugging.
* A one-shot deferred message indicating that a title created an AX-backed socket can be used to confirm routing without packet-level tracing.

### Native comparison mode

A diagnostic mode is available through:

```ini
route=native
```

This forces newly created game sockets back through the original Wii U network stack.

It is useful for comparison against:

```ini
route=ax
```

For native comparison tests to be meaningful, socket tracing must remain silent because the instrumentation itself can affect game behavior.

### Current compatibility milestone

As of 2026-09-18, the following path is validated:

```text
Minecraft: Wii U Edition
        ↓
nsysnet GAME-process hooks
        ↓
lwIP sockets
        ↓
AX88179 driver
        ↓
USB Ethernet
        ↓
Pretendo networking
        ↓
Minecraft gameplay / map creation
```

This is the first validated commercial-game networking result for the driver.

It does not yet prove compatibility with every Wii U title or with system services such as NSSL, the Wii U Browser, eShop, or Wii U Menu networking.



## Current validated state — 2026-09-17

This section describes the current implementation. Older sections below are kept as a development journal and may describe experiments or approaches that were later rejected.

### Title transition lifecycle

The AX network worker is stopped and recreated across title transitions. Keeping it alive across transitions was tested and rejected because it could remain attached to a transitional title and fail to bring the interface back correctly.

Current startup guards:

* `25000 ms` for the first worker of an Aroma session.
* `2000 ms` for subsequent title transitions.

The first guard avoids touching UHS during the initial Aroma/RPXLoader transitions. Later transitions only require the shorter 2-second guard.

### Fast warm PHY recovery

A major title-transition delay was traced to resetting the AX88179 PHY on every reopen.

The first open performs the normal PHY power reset and restarts autonegotiation. On later opens the driver validates BMCR/BMSR first. If the PHY is still valid, the reset is skipped and the existing link is retained. If validation fails, the driver falls back to the normal cold-reset path.

Observed timings:

* Cold open: about `754 ms`, with link UP after about `3130 ms`.
* Warm open: about `252 ms`, with link UP after about `13-15 ms`.

Together with the 2-second transition guard and cached DHCP configuration, normal Ethernet recovery after a title transition dropped from roughly 5.5 seconds to roughly 2.4 seconds.

### Session DHCP lease caching

Repeated DHCP negotiation after every title transition was unnecessary and added noticeable network bring-up time.

A session-level DHCP lease cache was therefore implemented. In `keep_first` mode, the first successful DHCP negotiation is performed normally and the resulting IPv4 configuration (address, netmask, and gateway) is retained in the resident Aroma module. When the network worker is recreated after a title transition, lwIP is still rebuilt from scratch, but the cached network configuration is restored instead of starting another DHCP negotiation.

This preserves the required stop/restart lifecycle of the network worker while making network availability after title transitions significantly faster.

Two DHCP modes are available through `SD:/wiiu/ax88179/config.ini`:

* `keep_first` — keep and reuse the first successful DHCP configuration for the current Aroma session.
* `always` — perform a new DHCP negotiation after every title transition.

The cached lease is RAM-only and is lost on a full console reboot, so a fresh DHCP negotiation is still performed when starting a new Aroma session.

**Result:** `keep_first` was successfully tested and the Ethernet interface recovers its IP substantially faster after title transitions.

## SHIM VALIDATED — TCP+UDP through the adapter (2026-09-17)

The `test_shim_boot` test now waits for the IP address **locally** (`socket()` probe: native fallback is identified by `errno=-1`, then `SO_MYADDR` must be non-zero — lwIP returns `0.0.0.0` until DHCP has completed), instead of performing a UDP round trip to the PC, which depended on the echo server.

Console result: `AX socket path ready (IP 192.168.2.190)`, followed by TCP echo PASS 32/504/1024/1400 and UDP echo PASS 32/504/1024/1400 in a loop, with 0 errors.

On the PC side, the echo server sees 100% of the traffic arriving from `192.168.2.190` (never `.124`).

The `nsysnet` interception is therefore functional for a homebrew application: TCP/UDP sockets go through the AX88179 adapter, with IP detection requiring no external dependency.

WUHB exit reminder: system HOME overlay -> `Quit`, nothing else.

---

## Goal

Make an AX88179 USB Ethernet adapter work in Wii U mode through a user-mode Aroma module, with networking available from the Wii U Menu, homebrew applications, and games.

## Validated state

* The AX88179 driver can initialize the adapter, start RX/TX, obtain a DHCP lease, and respond to ping.
* Aroma module `v0.2.2-shim-delay30` boots, waits 30 seconds, applies the IOSU/UHS patch, opens the AX88179, starts lwIP/DHCP, and installs the `nsysnet` hooks.
* Observed IP addresses: Wii U Wi-Fi `192.168.2.124`, AX88179 Ethernet `192.168.2.190`, PC `192.168.2.100`.
* The global shim successfully intercepts sockets from an Aroma homebrew application: packet capture confirmed outgoing packets from `192.168.2.190`.
* Previous WUHB capture: TCP SYN and UDP packets are sent toward `192.168.2.100:18879`; the test's networking issue was therefore not caused by a lack of transmission.
* Anonymous FTP works on the console when it is not frozen; uploads were tested to `/fs/vol/external01/wiiu/apps/...`.

## Current issue

The current blocker is on the Aroma WUHB launch/exit side, not the AX88179/DHCP side.

* `AX88179 Shim Test` can freeze the console when launching or exiting through HOME/MINUS.
* `AX Safe Exit`, a minimal WUHB with no networking, no shim, no SDL, and no ProcUI, displays a black screen and then stops responding.
* This isolates the bug: even an OSScreen + VPAD WUHB with a 15-second automatic exit does not exit cleanly.
* Ethernet ping may continue while the WUHB is frozen, meaning the AX module can remain alive despite the application freeze.

## Important files

* Aroma module: `wiiu_ethernet/aroma_module/`
* Socket shim: `wiiu_ethernet/aroma_module/nsysnet_shim.c`
* Shim WUHB test: `wiiu_ethernet/test_shim_boot/`
* Minimal test that also freezes: `wiiu_ethernet/safe_exit_test/`
* PC echo server: `wiiu_ethernet/test_shim_boot/echo_server.py`

## Current module details

* Module version: `0.2.2-shim-delay30`.
* The worker waits 30 seconds before touching UHS to avoid breaking wiiload/RPXLoader during transfers.
* Native-side ports reserved by the shim: FTP `21`, wiiload `4299`.
* Despite these exclusions, wiiload remains fragile with global hooks enabled.

## Latest test added

`safe_exit_test` was created to isolate the WUHB issue:

* Aroma app: `AX Safe Exit`
* SD: `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`
* SHA1: `d5df9f3da7a26b404307be9b4bfe02cd21798405`
* Size: `56652` bytes
* Code: OSScreen + VPAD, no networking, no shim, no SDL, no ProcUI
* User result: black screen, console frozen

## Safe Exit v2 deployed

After the first `safe_exit_test` froze, it was compared with `wiiu_console/bringup`: the minimal test was not using the known ProcUI/VPAD skeleton from the repository.

Changes applied:

* `safe_exit_test` now uses `ProcUIInitEx` through `proc.c/proc.h`, taken from `wiiu_console/bringup/src/`.
* Explicit `VPADInit()` added.
* OSScreen + text + HOME/MINUS/PLUS/B + 15-second auto-exit retained.
* New WUHB copied and verified on SD: `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`.
* SHA1 v2: `8f2e52db8d6542ac7a4df7276246d4702488a148`, size `62348` bytes.

To test: launch the `AX Safe Exit` icon again. If v2 exits cleanly, resume WUHB networking tests using this skeleton.

## Safe Exit v2 console result

User test: `AX Safe Exit v2` launched from Aroma.

Observed display:

```text
AX Safe Exit 2 - ProcUI skeleton
err=0 hold=00000000 trig=00000000
auto exit in 7 s
HOME/MINUS/PLUS/B exits
```

Observed UDP logs:

```text
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
AX88179 module: worker stopped, interface released
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
AX88179 module: IOSU patch applied
AX88179 module: worker stuck, leaving it to the process teardown
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
ax_safe_exit2: starting
ax_safe_exit2: frame=60 elapsed=2
ax_safe_exit2: frame=120 elapsed=4
ax_safe_exit2: frame=180 elapsed=7
ax_safe_exit2: frame=240 elapsed=9
ax_safe_exit2: frame=300 elapsed=11
ax_safe_exit2: frame=360 elapsed=14
ax_safe_exit2: auto exit
ax_safe_exit2: auto exit
AX88179 module: worker stopped, interface released
ax_safe_exit2: done
```

Conclusion:

* The v2 WUHB skeleton works correctly for display, VPAD, and the main loop.
* The application reaches `ax_safe_exit2: done`, so the freeze is no longer caused by application code before `return 0`.
* After exiting, the console remains frozen on the menu + logo and Ethernet ping stops responding.
* The remaining issue is probably in the Aroma/ProcUI/title-return transition or in the teardown/restart of the Aroma module around application shutdown.
* The `worker stuck, leaving it to the process teardown` log is suspicious: the AX worker shutdown needs to be fixed before continuing shim networking tests.

Recommended next action:

1. Make worker shutdown deterministic: stop flag, wake RX, short timeout, and do not leave a thread blocked in UHS/RX during `APPLICATION_ENDS`.
2. Test `AX Safe Exit v2` with the AX module disabled or with the shim/worker not started, to distinguish a pure ProcUI issue from a module teardown issue.
3. If returning to the menu works without the module, fix `aroma_module` before performing any new WUHB networking tests.

## Module v0.2.3-persist-worker deployed

Change made after the freeze following `ax_safe_exit2: done`:

* The AX worker is no longer stopped during `WUMS_APPLICATION_REQUESTS_EXIT` or `WUMS_APPLICATION_ENDS`.
* These hooks only stop shim acceptance and log that the worker remains alive.
* `WUMS_APPLICATION_STARTS` now calls `nsysnet_shim_begin_title()` even if the worker is already running.
* `stop_worker()` remains used during `WUMS_DEINITIALIZE`.
* Goal: avoid UHS/RX blocking during Aroma/WUHB transitions, suspected because of `worker stuck, leaving it to the process teardown`.
* WMS copied and verified on SD: `/wiiu/environments/aroma/modules/AX88179Module.wms`.
* SHA1: `ec8a3d6386886ef2af06f9211d2788f84e3f8773`.

To test after reboot: launch `AX Safe Exit`, wait for auto-exit or exit with B/MINUS, then verify menu return and ping.

## Safe Exit v3 deployed

Correction after clarification: menu boot works; the freeze occurs when exiting `AX Safe Exit`.

Probable cause found: `safe_exit_test` v2 reused the old `proc.c` from `wiiu_console/bringup`, which calls `SYSRelaunchTitle()` inside `proc_shutdown()`. This path had already been identified as blocking in the Aroma/Health & Safety context.

v3 changes:

* `safe_exit_test/proc.c` and `proc.h` replaced with the corrected version from `wiiu_ethernet/common/`.
* `proc_shutdown()` no longer relaunches the title; it logs and then returns from `main`.
* Display indicates `AX Safe Exit v3 - no relaunch`.
* WUHB copied and verified on SD: `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`.
* SHA1 v3: `82be508073d8bddd2bb85a79e1961b133895fc00`.

To test: launch `AX Safe Exit`, wait for auto-exit or exit with B/MINUS, then check whether the menu returns without freezing and whether ping remains active.

## Safe Exit v4 deployed

v3 result: the application reaches `AXPROBE ProcUI: shutdown end` and then `ax_safe_exit3: done`, but remains on a black screen. Therefore, execution continues until after `ProcUIShutdown`, but system return does not resume.

v4 changes:

* `proc_stop()` still calls `SYSLaunchMenu()` to explicitly request a return to the menu.
* `SYSRelaunchTitle()` remains forbidden.
* `g_running` is then set to 0 to cleanly exit the loop.
* Screen: `AX Safe Exit v4 - launch menu`.
* SHA1 v4: `07fb64fcab6b597fdeea4e46fee5a4d2940de81b`.

## Safe Exit v5 ready but not yet deployed

v4 result:

```text
ax_safe_exit4: auto exit
AXPROBE ProcUI: SYSLaunchMenu begin
AXPROBE ProcUI: SYSLaunchMenu returned
AXPROBE ProcUI: status=2
AXPROBE ProcUI: shutdown begin
AXPROBE ProcUI: shutdown end
ax_safe_exit4: done
```

The console then remains on a black screen.

Interpretation: v4 calls `ProcUIShutdown()` too early, immediately after `PROCUI_STATUS_RELEASE_FOREGROUND` (status 2).

Local v5 changes:

* `proc_stop()` calls `SYSLaunchMenu()` but no longer sets `g_running=0`.
* The code continues pumping `ProcUIProcessMessages()` after status 2.
* `ProcUIDrawDoneRelease()` can release the foreground, after which the code waits for `PROCUI_STATUS_EXITING` before calling `ProcUIShutdown()`.
* Local v5 SHA1: `2a58403af03cdb06838d1bd4d3752568e8d78762`.
* FTP upload not performed because the console was still frozen and `192.168.2.124:21` was no longer responding.

## Module no-op isolation deployed

v5 result: ProcUI reaches `status=3`, `ProcUIShutdown end`, then `ax_safe_exit5: done`, but the menu remains frozen while restarting. This proves that the WUHB exits correctly all the way to completion.

Next test deployed to SD: Aroma module `0.2.4-noop-isolation`.

* No AX worker.
* No shim.
* WUMS hooks only log events.
* WMS SHA1: `6edab878cb2c13f73b4d5d7d45d15422e740b4d1`.
* Goal: launch `AX Safe Exit v5` with a neutral module. If the menu returns, the freeze comes from the AX worker/shim. If it still freezes, the issue is outside the AX module.

## No-op isolation result

With Aroma module `0.2.4-noop-isolation`:

```text
AX88179 module: noop isolation loaded v0.2.4
AX88179 module: noop app starts
AX88179 module: noop app requests exit
AX88179 module: noop app ends
ax_safe_exit5: starting
AXPROBE ProcUI: status=0
...
AXPROBE ProcUI: SYSLaunchMenu returned
AXPROBE ProcUI: status=2
AXPROBE ProcUI: status=3
AXPROBE ProcUI: shutdown begin
AXPROBE ProcUI: shutdown end
ax_safe_exit5: done
```

The console remains frozen while restarting the menu.

Conclusion: the freeze is not caused by the AX worker, the shim, or UHS. It is in the WUHB/Aroma/ProcUI exit sequence.

Display detail: the `err=0 hold=00000000 trigger=00000000` line is normal. It comes from text drawn on screen by `safe_exit_test/main.c`, not from UDP logs.

Next test: v6 should treat the Aroma/Health & Safety context as a borrowed title and use `SYSRelaunchTitle()` after `ProcUIShutdown`, while keeping the no-op module active to test this exit path without AX/shim.

## Safe Exit v6 result

User result: `SYSRelaunchTitle()` does not return to the Aroma menu; it relaunches the test application itself.

Conclusion: `SYSRelaunchTitle()` is rejected for this WUHB Aroma context.

The tested approaches are now:

* Simple return after `ProcUIShutdown`: black screen.
* `SYSLaunchMenu()` followed by waiting for `EXITING`: frozen while restarting the menu.
* `SYSRelaunchTitle()`: relaunches the application itself.

Next approach: test a WUHB without `SYSLaunchMenu` or `SYSRelaunchTitle`, which only requests ProcUI shutdown and returns, but using a sequence closer to Aroma/HBL examples if one can be found.

## Safe Exit v7 deployed

After v6: `SYSRelaunchTitle()` relaunches the application itself, so this approach is rejected.

v7 changes:

* Exit through `_SYSDirectlyReturnToCaller()` after `ProcUIShutdown`.
* No `SYSLaunchMenu()`.
* No `SYSRelaunchTitle()`.
* Screen: `AX Safe Exit v7 - direct return`.
* SHA1 v7: `b8edd704c2ee68589e2b70896609e21afc389473`.

## Safe Exit v8 deployed

Observation: previous tests exited correctly (`2026-09-16-probe-home-direct-first/second`). They used the `probe_init` / `probe_poll` / `probe_shutdown` flow, with `ui_shutdown()` before `proc_shutdown()`.

`safe_exit_test` v1-v7 used a custom OSScreen path and diverged from this flow.

v8 changes:

* `safe_exit_test` now reuses `common/probe.c`, `common/ui.c`, and `common/proc.c`.
* `main` is reduced to `probe_init`, a `probe_poll` loop, auto-exit, then `probe_shutdown`.
* This therefore uses the same flow as the tests that had already successfully returned to the menu twice.
* SHA1 v8: `4fa11d2aaa973e67b7dc54e46ce490053d72b0e6`.

## Safe Exit v9 deployed

Method correction after comparison with `capture2wiiu`: capture2wiiu exits through the system HOME overlay followed by the `Quit` button, not through auto-exit or a forced exit.

v9 changes:

* HOME is no longer intercepted.
* No auto-exit.
* No `SYSLaunchMenu`, `SYSRelaunchTitle`, or `_SYSDirectlyReturnToCaller`.
* Normal ProcUI, with the system HOME overlay allowed.
* Test instruction: press HOME, then choose `Quit` in the Wii U interface.
* SHA1 v9: `0cc7c011637b7dbbaa45016d6db68a507e5e00f4`.

## Safe Exit v9 result and normal module restored

User result: v9 works.

The correct exit path for Aroma WUHB applications is the official HOME overlay followed by the `Quit` button. The forced exit methods tested previously were the actual problem.

Conclusion:

* Do not intercept HOME to exit normal test WUHB applications.
* Allow the system HOME Menu overlay to open.
* The user selects `Quit`.
* Auto-exit, `SYSLaunchMenu`, `SYSRelaunchTitle`, and `_SYSDirectlyReturnToCaller` must not be used as the primary exit path.

Normal AX module restored on SD:

* Version: `0.2.3-persist-worker`.
* Path: `/wiiu/environments/aroma/modules/AX88179Module.wms`.
* SHA1: `54da8e746bdce256879aee8b0219ad29b74fe417`.
* The no-op module remains as a backup under `AX88179Module.wms.off`.

Next test: reboot with the normal module, wait for DHCP/ping, launch the tests, and exit through the official HOME overlay -> `Quit`.

## Module v0.2.5-restart-worker deployed

Result with `0.2.3-persist-worker`: at the menu, logs only show:

```text
AX88179 module: worker started v0.2.3-persist-worker ...
AX88179 module: app requests exit, worker kept alive
AX88179 module: app ended, worker kept alive
```

No Ethernet IP address appears afterward.

Conclusion: keeping the worker alive across an Aroma transition does not work; the thread starts inside a transitional title and never reaches DHCP.

Changes in `0.2.5-restart-worker`:

* Return to stop/restart behavior on `WUMS_APPLICATION_REQUESTS_EXIT` and `WUMS_APPLICATION_ENDS`.
* Keep the correct WUHB exit method: official HOME overlay -> `Quit`.
* WMS copied and verified on SD.
* SHA1: `ec95ecf61f6f06b9e492fa90ba33044ee045968a`.

To test after reboot: wait for the `worker started v0.2.5`, `IOSU patch`, and DHCP BOUND logs, then ping `192.168.2.190`.

## Module v0.2.6-keep-ready deployed

Result from the first `AX88179 Shim Test` with v0.2.5: TCP/UDP echo PASS, but the PASS messages occur before the new `DHCP BOUND`.

This may be a false positive through Wi-Fi/native fallback because v0.2.5 stops the worker during the title transition and then waits 30 seconds before reopening the AX adapter.

v0.2.6 changes:

* If `ax_net_stack_ready()` is true during `APPLICATION_REQUESTS_EXIT/ENDS`, do not stop the worker; only call `nsysnet_shim_stop_accepting()` during the transition.
* On the next `APPLICATION_STARTS`, `nsysnet_shim_begin_title()` re-enables acceptance.
* If the stack is not ready, keep the previous stop/restart behavior to avoid the `persist-worker` bug in transitional titles.
* Relative timestamps (`[ms]`) added to module logs.
* WMS SHA1: `7d18b6494b78b9cce41769ae267ac98f31d8eadf`.

To test: reboot, wait for DHCP on the menu, launch `AX88179 Shim Test`, verify that the worker remains alive and that packets to port `18879` originate from `192.168.2.190`.

## Recommended next approach

1. Stop WUHB networking tests until the basic WUHB foundation exits cleanly.
2. Find a known-working Aroma WUHB in the repository or on the SD card, then reproduce its exact init/loop/exit skeleton.
3. Compare it with `safe_exit_test`, especially screen initialization, ProcUI, title/app metadata, and Makefile/WUHB rules.
4. If direct OSScreen usage causes problems under Aroma, replace it with the UI skeleton already validated in another homebrew.
5. Once a minimal WUHB that launches and exits cleanly has been validated, reintroduce features step by step: on-screen logs, then native sockets, then the shim, then TCP/UDP tests.

## Useful commands

FTP upload:

```sh
curl -sS --fail --ftp-create-dirs -T <file.wuhb> \
  ftp://anonymous:@192.168.2.124/fs/vol/external01/wiiu/apps/<app>/<app>.wuhb
```

Check the PC echo server:

```sh
ss -ltnup | grep 18879 || python3 -u wiiu_ethernet/test_shim_boot/echo_server.py >/tmp/ax-boot-echo.log 2>&1 &
```

Warning: do not use `pgrep -af 'echo_server.py' || ...`, because it may match its own shell command and therefore fail to start the server.


## Multi-datagram nsysnet ABI

Native Wii U and AX shim behavior have been validated for
`sendto_multi`, `sendto_multi_ex` and `recvfrom_multi`.

### IPC alignment

The multi-datagram APIs use IOS-style buffers:

- pointers aligned to `0x40`
- exposed buffer lengths padded to `0x40`

### sendto_multi

One payload sent to two destinations:

- payload length: 10
- return value: 10
- both destinations receive the complete payload

Native nsysnet and AX/lwIP behave identically.

### sendto_multi_ex

For two datagrams of 4 and 8 bytes:

- `send_datagram_count = 2`
- buffer capacities padded to `0x40`
- return value = `12`
- `results[0] = 4`
- `results[1] = 8`

Native nsysnet and AX/lwIP behave identically.

### recvfrom_multi

For three datagrams of 6, 8 and 12 bytes:

- `recv_datagram_len = 64`
- `recv_datagram_count = 3`
- return value = `3`
- `results[] = { 6, 8, 12 }`
- one source address is returned per datagram

Native nsysnet and AX/lwIP behave identically.

### recvfrom_multi timeout ABI

Raw nsysnet does not use newlib's `struct timeval` layout.

Observed:

- `sizeof(struct timeval) = 16`
- raw nsysnet timeout size = 8

Raw layout:

    struct nsysnet_timeval {
        int32_t tv_sec;
        int32_t tv_usec;
    };

Passing libc `struct timeval` directly to the raw export produced an
effective immediate timeout.


## recvfrom_ex TTL probe v1 - blocking harness

First native `recvfrom_ex` TTL characterization attempt.

Observed before the hang:

- module configuration: `route=native`
- AX stack itself remained healthy at `192.168.2.190`
- probe socket path: `192.168.2.124:19030`
- raw nsysnet descriptor: `6`
- execution reached `baseline: no MSG_IP_RECVTTL`

The probe then blocked inside its first blocking `recvfrom_ex()` call.

Because the call was made on the application's main/ProcUI thread,
`probe_poll()` stopped running as well. Consequently the normal HOME
overlay -> Quit path could no longer complete.

Conclusion:

- this run is inconclusive for the `recvfrom_ex` TTL ABI
- it does confirm that the intended native socket path was selected
- this is a probe/harness bug, not evidence of an nsysnet or AX bug
- future socket probes must never make an unbounded blocking network call
  on the ProcUI thread

Fix: wait for readability while pumping ProcUI and keep the tested socket
nonblocking before invoking the raw nsysnet export.


## recvfrom_ex native TTL ABI

`recvfrom_ex` was characterized on the native Wii U nsysnet path.

Test path:

- route: `native`
- local socket: `192.168.2.124:19030`
- peer: `192.168.2.100:19031`
- raw nsysnet fd: `6`

The PC transmitted UDP datagrams with explicitly controlled IPv4 TTL
values.

### Without MSG_IP_RECVTTL

With:

- flags = `0`
- msglen = `64`

the datagram was received normally and the entire supplied `extra`
buffer was cleared to zero.

Example:

    payload TTL=91
    extra = 00 00 00 00 ...

Therefore `extra` is an output buffer which nsysnet initializes on a
successful receive even when TTL reporting was not requested.

### MSG_IP_RECVTTL

Flag `0x40` is confirmed to request the received IPv4 TTL.

For every tested `msglen >= 1`, the first byte of `extra` contained the
actual received packet TTL:

    payload TTL=127 -> extra[0]=0x7f
    payload TTL=200 -> extra[0]=0xc8
    payload TTL=17  -> extra[0]=0x11
    payload TTL=37  -> extra[0]=0x25
    payload TTL=64  -> extra[0]=0x40
    payload TTL=91  -> extra[0]=0x5b

For msglen values 2, 4, 8, 16 and 64, all remaining bytes inside the
provided output length were zero.

Observed contract on successful receive:

    memset(extra, 0, msglen);

    if ((flags & 0x40) && msglen >= 1)
        extra[0] = received_ipv4_ttl;

### msglen=0

One nonblocking test with `flags=0x40, msglen=0` returned:

    rc=-1
    socketlasterr=11

The datagram was not consumed.

This edge case is not considered fully characterized yet because the
test uses readiness polling plus a nonblocking raw nsysnet call. It must
not be used to infer an EINVAL/EWOULDBLOCK ABI rule without a dedicated
test.

### Probe lifecycle

The corrected nonblocking probe exited normally through the official
HOME overlay -> Quit path.


## recvfrom_ex AX TTL validation

The native `recvfrom_ex` TTL behaviour has now been reproduced on the
AX88179/lwIP path.

Observed AX path:

- module route: `ax`
- AX address: `192.168.2.190`
- title traffic explicitly reported as routed through AX88179
- UDP source peer: `192.168.2.100:19031`

### recvfrom_ex results

Baseline without `MSG_IP_RECVTTL`:

    flags=0
    msglen=64
    extra = all zero

TTL cases:

    expected 127 -> extra[0] = 0x7f
    expected 200 -> extra[0] = 0xc8
    expected 17  -> extra[0] = 0x11
    expected 37  -> extra[0] = 0x25
    expected 64  -> extra[0] = 0x40
    expected 91  -> extra[0] = 0x5b

For msglen > 1, bytes `extra[1..msglen-1]` are zero.

Therefore the real per-datagram IPv4 TTL is successfully preserved
through lwIP and exposed by the AX `recvfrom_ex` shim.

`recvfrom_ex` TTL support is considered validated for `msglen >= 1`.

### Remaining msglen=0 edge case

AX currently receives the datagram successfully with:

    flags=0x40
    msglen=0
    rc > 0

The single native observation instead returned `socketlasterr=11`
without consuming the datagram.

This edge case remains pending dedicated characterization and is not yet
considered ABI-compatible.

### New getsockname discrepancy

The same connected UDP probe reported:

Native:

    PATH local=192.168.2.124:19030

AX:

    PATH local=0.0.0.0:19030

Traffic itself was routed correctly through AX88179, but lwIP preserves
the wildcard bind address in `getsockname()` after UDP connect whereas
native nsysnet exposes the selected local interface address.

This is a separate observable compatibility gap and should be fixed
without changing wildcard results for unconnected sockets.


## Connected getsockname AX validation

After the connected-local-address compatibility fix, the AX
`recvfrom_ex` probe reported:

    PATH local=192.168.2.190:19030

The equivalent native test reported:

    PATH local=192.168.2.124:19030

Therefore an AX UDP socket bound to `INADDR_ANY` and then connected now
exposes the selected AX interface address through `getsockname()`, like
native nsysnet.

The recvfrom_ex TTL behaviour remained correct after this change:

- baseline extra buffer is zero-filled
- msglen >= 1 returns the real received IPv4 TTL in extra[0]
- remaining bytes inside msglen are zero

Observed AX TTL matches:

    200 -> c8
     17 -> 11
     37 -> 25
     64 -> 40
     91 -> 5b
    127 -> 7f

No regression was observed in the validated recvfrom_ex TTL path.

One remaining discrepancy is isolated:

Native observation:
    MSG_IP_RECVTTL + msglen=0 -> rc=-1, socketlasterr=11

AX observation:
    MSG_IP_RECVTTL + msglen=0 -> datagram received successfully

This zero-length metadata case requires dedicated characterization.


## recvfrom_ex zero-length metadata native ABI

Dedicated native characterization of `recvfrom_ex` with `extra_len=0`
resolved the remaining ambiguity.

Path:

    route=native
    local=192.168.2.124:19030

Observed baseline receive first succeeded normally.

Then:

    flags=0x00, extra_len=0
        rc=-1
        socketlasterr=11

    flags=0x40, extra_len=0
        rc=-1
        socketlasterr=11

The `MSG_IP_RECVTTL + extra_len=0` case was repeated three times and
returned the same result every time.

In the shim's established nsysnet errno mapping:

    nsysnet error 11 = EINVAL

Therefore the native contract is:

    recvfrom_ex(..., extra_len=0)
        -> -1
        -> EINVAL / socketlasterr 11
        -> queued datagram is not consumed

This rule is independent of MSG_IP_RECVTTL.

The following `extra_len=1` receive succeeded immediately and returned
the expected TTL, confirming that the zero-length calls rejected the
operation before consuming the pending UDP datagram.

For `extra_len >= 1`, the previously characterized behaviour remains:

- output buffer is zero-filled through `extra_len`
- with MSG_IP_RECVTTL, `extra[0]` contains the real received IPv4 TTL
- datagram receive proceeds normally


## recvfrom_ex AX final parity validation

Final AX comparison after implementing the native `extra_len=0`
semantics.

Path:

    route=ax
    local=192.168.2.190:19030

Baseline:

    flags=0
    extra_len=64
    receive succeeds
    extra buffer is zero-filled

Zero-length metadata:

    flags=0, extra_len=0
        rc=-1
        socketlasterr=11

    flags=MSG_IP_RECVTTL, extra_len=0
        rc=-1
        socketlasterr=11

The TTL/zero-length case was repeated three times with the same result.

The output sentinel remained unchanged, and the following successful
receive proved that the rejected calls did not consume the queued UDP
datagram.

For every tested extra_len >= 1, the actual received IPv4 TTL matched
the PC-controlled packet TTL exactly.

Observed examples:

    200 -> c8
     17 -> 11
     37 -> 25
     64 -> 40
     91 -> 5b
    127 -> 7f

The connected UDP socket also reports:

    getsockname = 192.168.2.190:19030

Conclusion:

`recvfrom_ex` now matches the characterized native nsysnet behaviour for
the tested IPv4 UDP cases, including its zero-length metadata error path.


## TCP server native reference validation

A full TCP server/listen/accept reference test was completed on native
nsysnet before validating the AX/lwIP implementation.

Path:

    route=native
    interface=192.168.2.124
    listener=192.168.2.124:19010

Listener:

    SO_TYPE=SOCK_STREAM
    listen(backlog=2) succeeded

Two TCP clients were connected before either was serviced, validating
that the native listener can queue at least the two connections used by
the test.

Accepted sockets reported correct endpoint information:

    local=192.168.2.124:19010
    peer=192.168.2.100:<ephemeral port>
    SO_TYPE=SOCK_STREAM

Both short connections exchanged their expected messages successfully.

Important observation:

The order of application payloads did not correspond to the order of
the accepted descriptors:

    first accepted socket  -> CLIENT2
    second accepted socket -> CLIENT1

Therefore client identity/order must not be inferred from accept order
when comparing native and AX behaviour. Only connection/data semantics
should be compared.

### Bulk / half-close test

A third TCP connection transferred:

    131072 bytes

Computed FNV-1a:

    received = 37309dc5
    expected = 37309dc5

The client then performed:

    shutdown(SHUT_WR)

Native recv() returned EOF:

    eof=1

The server was still able to transmit its reply after receiving EOF,
confirming the expected TCP half-close semantics.

The server then successfully performed:

    shutdown(SHUT_WR) -> rc=0

Final native result:

    BACKLOG/SHORT CONNECTIONS: PASS
    BULK DATA + HALF-CLOSE: PASS
    TCP SERVER RESULT: PASS

This is now the reference behaviour for the equivalent AX/lwIP test.


## TCP server AX parity validation

The native TCP server reference test was repeated unchanged through the
AX88179/lwIP shim.

Path:

    route=ax
    interface=192.168.2.190
    listener=192.168.2.190:19010

### Listen / accept

The listener successfully used:

    socket(AF_INET, SOCK_STREAM)
    bind(INADDR_ANY, 19010)
    listen(backlog=2)

Two clients connected before either connection was serviced.

Both were accepted successfully:

    accepted fd=5
    accepted fd=6

Accepted socket endpoint information was correct:

    local=192.168.2.190:19010
    peer=192.168.2.100:<ephemeral port>
    SO_TYPE=SOCK_STREAM

Both short request/reply exchanges succeeded.

Result:

    BACKLOG/SHORT CONNECTIONS: PASS

### Bulk transfer / half-close

A third accepted TCP connection received:

    131072 bytes

FNV-1a integrity:

    received = 37309dc5
    expected = 37309dc5

The PC then performed:

    shutdown(SHUT_WR)

The AX-side recv() correctly returned EOF:

    eof=1

The Wii U was still able to send the `BULK-OK` response after receiving
EOF, confirming correct TCP half-close semantics.

The Wii U then performed:

    shutdown(SHUT_WR) -> rc=0

The PC received the reply and then observed:

    recv() = b''

confirming the server-side FIN.

Final results:

    BACKLOG/SHORT CONNECTIONS: PASS
    BULK DATA + HALF-CLOSE: PASS
    TCP SERVER RESULT: PASS
    TCP SERVER PEER: PASS

Conclusion:

The tested native nsysnet TCP server behaviour is reproduced by the
AX/lwIP shim for listen, backlog, accept, endpoint reporting, ordinary
RX/TX, 128 KiB transfer and bidirectional TCP half-close semantics.


## Socket exhaustion AX characterization

The socket exhaustion probe was first run through the AX/lwIP path.

Path:

    route=ax
    SO_MYADDR=192.168.2.190

The result was perfectly stable across three complete allocate/free
cycles.

### TCP

Each round produced:

    count=8
    public fd range=4..11
    fd mask=0x00000ff0

The next socket() failed with:

    errno=105
    socketlasterr=1

In the established nsysnet error mapping:

    errno 105 = ENOBUFS
    nsysnet error 1 = ENOBUFS

All eight sockets then closed successfully:

    close errors=0

An immediate new TCP socket succeeded again:

    fd=4
    errno=0

### UDP

Each round produced:

    count=7
    public fd range=4..10
    fd mask=0x000007f0

The next socket() failed with:

    errno=105
    socketlasterr=1

All seven sockets closed successfully and an immediate new UDP socket
again returned fd 4.

### Mixed TCP/UDP

Alternating TCP and UDP sockets produced:

    total=15
    TCP=8
    UDP=7
    public fd mask=0x0007fff0

Saturation again produced:

    errno=105 / ENOBUFS
    socketlasterr=1 / ENOBUFS

All 15 descriptors closed without error.

Immediately afterwards both socket types could be allocated again:

    TCP fd=4
    UDP fd=5

### Reclamation / leak result

All three rounds were identical:

    TCP   8, 8, 8
    UDP   7, 7, 7
    MIXED 15, 15, 15

No progressive capacity loss was observed.

Therefore no socket/PCB/netconn leak is visible across the tested
allocate -> exhaust -> close -> reallocate cycles.

### Relation to current lwIP pools

The current stack is configured with:

    MEMP_NUM_TCP_PCB=8
    MEMP_NUM_UDP_PCB=8
    MEMP_NUM_NETCONN=16

The measured AX limits correspond closely to these configured resource
pools.

One UDP/netconn-sized resource appears to already be occupied during
the probe. WHB UDP logging uses a persistent UDP socket, so logging
infrastructure is a likely contributor. This must not be treated as a
final explanation until compared with the native reference and, if
needed, isolated explicitly.

This AX run is a characterization only. Socket exhaustion compatibility
is not marked validated until the identical probe is run on native
nsysnet.


## Socket exhaustion AX repeat validation

A second socket-exhaustion run was performed, but the module was still
in AX routing mode rather than native reference mode.

Confirmed path:

    title traffic routed through AX88179
    SO_MYADDR=192.168.2.190

The result exactly reproduced the previous AX run for all three rounds:

    TCP   = 8 sockets
    UDP   = 7 sockets
    MIXED = 15 sockets (8 TCP + 7 UDP)

Every saturation point returned:

    errno=105 = ENOBUFS
    socketlasterr=1 = ENOBUFS

Every close sequence completed with zero errors.

Immediate socket reallocation after exhaustion succeeded in every round.

This independently reconfirms that the current AX socket pools are
stable across repeated exhaustion/reclamation cycles and show no
progressive resource leak.

This run is NOT the native reference because both the routing log and
SO_MYADDR confirmed the AX path.


## Socket exhaustion native reference

The socket exhaustion probe was run through the original Wii U nsysnet
stack.

Path:

    route=native
    SO_MYADDR=192.168.2.124

The result was identical across all three test rounds.

### TCP

Native nsysnet allowed:

    count=28
    descriptors=4..31
    mask=0xfffffff0

The next socket() failed with:

    errno=24
    socketlasterr=51

This corresponds to:

    EMFILE

All 28 sockets closed successfully and a new socket could immediately
be allocated again as fd 4.

### UDP

Native nsysnet also allowed:

    count=28
    descriptors=4..31

The next socket() again failed with:

    errno=24
    socketlasterr=51
    error=EMFILE

### Mixed TCP/UDP

Alternating socket types produced:

    total=28
    TCP=14
    UDP=14
    descriptors=4..31

The 29th socket again failed with EMFILE.

### Interpretation

The native limit is therefore primarily the public nsysnet descriptor
table rather than separate TCP or UDP protocol limits:

    fd 0..3  unavailable/reserved
    fd 4..31 available to the title
    total title capacity = 28 sockets

The behaviour was perfectly stable for three complete
allocate -> exhaust -> close -> reallocate cycles, with no observed
resource leak.

### AX mismatch discovered

The current AX/lwIP configuration only provides:

    TCP=8
    UDP=7
    mixed=15

and currently reaches ENOBUFS before the native descriptor limit.

To reproduce native behaviour, the lwIP socket/PCB pools must provide
enough internal capacity for at least 28 title sockets plus internal
stack/logging sockets. The public native placeholder descriptors should
then naturally impose the correct 28-socket / EMFILE limit.


## Socket exhaustion AX/native parity validation

After increasing the lwIP socket resource pools and removing the
artificial internal descriptor offset, the socket exhaustion probe was
repeated through the AX path.

The result now exactly matches the native nsysnet reference.

### Native reference

    TCP:
        count=28
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

    UDP:
        count=28
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

    MIXED:
        total=28
        TCP=14
        UDP=14
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

    errno 24 = EMFILE
    nsysnet error 51 = EMFILE

### AX result

After configuring:

    MEMP_NUM_TCP_PCB=32
    MEMP_NUM_UDP_PCB=32
    MEMP_NUM_NETCONN=32
    LWIP_SOCKET_OFFSET=0

the AX path produced exactly:

    TCP:
        count=28
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

    UDP:
        count=28
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

    MIXED:
        total=28
        TCP=14
        UDP=14
        descriptors=4..31
        saturation errno=24
        socketlasterr=51

All closes succeeded and socket allocation resumed immediately after
each exhaustion cycle.

Three complete rounds were identical.

### Repeated AX validation

The AX exhaustion test was also executed both while the optional PC
Python networking helper was absent and while it was running.

The results were identical.

This is expected because the exhaustion probe only creates unconnected
sockets; it does not communicate with the PC peer.

### Conclusion

The title-visible socket capacity now matches native nsysnet:

    usable public descriptors = 4..31
    capacity = 28 sockets
    29th socket -> EMFILE / socketlasterr 51

The internal lwIP pools now provide enough headroom that the native
public descriptor table, rather than lwIP resource exhaustion, imposes
the externally visible limit.

No progressive socket resource leak was observed across repeated
exhaustion/reclamation cycles.


## LWIP_SOCKET_OFFSET=0 regression run

After increasing the lwIP socket pools and changing:

    LWIP_SOCKET_OFFSET=16 -> 0

the ordinary shim probe was run through AX.

Validated results:

    TCP echo 32 PASS
    TCP echo 504 PASS
    TCP echo 1024 PASS
    TCP echo 1400 PASS

    UDP echo 32 PASS
    UDP echo 504 PASS
    UDP echo 1024 PASS
    UDP echo 1400 PASS

    TCP concurrent rounds=32/32
    UDP concurrent rounds=32/32
    AXCONCURRENT result=PASS

This confirms that ordinary TCP/UDP traffic and concurrent socket use
still work with private lwIP descriptors in the 0..31 range.

However the run ended with:

    AXPROBE release=-5

Investigation showed that the probe harness predates automatic global
title routing.

The socket named `native_fd` was created only before
`AXShimBeginProbe()`, but by that time the production shim was already
installed and accepting AX sockets. Therefore that descriptor was no
longer guaranteed to be native.

Consequently the printed:

    native socket + mixed poll: PASS

must NOT be considered a valid native/AX coexistence validation for this
run.

`AXShimEndProbe()` also used the old assumption that open_mask must be
zero, although automatic routing means module/title AX sockets may
already exist before BeginProbe.

The harness must be updated to:

1. create its native coexistence socket before the module worker can
   install the shim;
2. close that socket before EndProbe;
3. make EndProbe compare against resources which already existed when
   BeginProbe started rather than requiring global open_mask == 0;
4. print the coexistence socket's actual local address.


## Native/AX coexistence and fd remapping final validation

The shim probe was corrected to create its deliberately native socket
before the AX worker installs the production hooks, and probe resource
ownership is now compared against the state which existed at
AXShimBeginProbe().

Final run:

    early native candidate fd=3

Ordinary shim TCP socket:

    local=192.168.2.190
    32 bytes   PASS
    504 bytes  PASS
    1024 bytes PASS
    1400 bytes PASS

Ordinary shim UDP socket:

    local=192.168.2.190
    32 bytes   PASS
    504 bytes  PASS
    1024 bytes PASS
    1400 bytes PASS

The coexistence socket was independently proven to remain on the native
Wii U interface:

    AXCONCURRENT coexist socket local=192.168.2.124:2210

while the concurrent shim sockets used:

    AX88179 = 192.168.2.190

A poll containing both native and AX-backed descriptors succeeded:

    AXCONCURRENT native socket + mixed poll: PASS

Concurrent workload:

    TCP rounds=32/32 bytes=23680 errno=0
    UDP rounds=32/32 bytes=23680 errno=0
    AXCONCURRENT result=PASS

Probe teardown:

    AXPROBE release=0

This confirms that no resources created by the diagnostic remained
owned when AXShimEndProbe() was called.

### Conclusion

LWIP_SOCKET_OFFSET=0 is validated with the shim's explicit
public-fd -> lwIP-fd mapping.

The following are now jointly validated:

- native public fd space 4..31
- native-compatible 28 socket exhaustion limit
- EMFILE/socketlasterr=51 at the 29th public socket
- internal lwIP descriptors starting at zero
- TCP and UDP operation
- mixed native/AX poll
- simultaneous native and AX routing in one title
- concurrent TCP/UDP activity
- clean probe resource reclamation

The earlier `release=-5` and supposed native coexistence result were
probe-harness issues and are superseded by this corrected validation.


## Native SNDBUF/RCVBUF and TCP backpressure characterization

The dedicated backpressure probe was first run through native nsysnet.

Path:

    route=native
    SO_MYADDR=192.168.2.124

### Negative SNDBUF range

All tested negative signed 32-bit values were accepted by setsockopt()
and returned unchanged by getsockopt():

    INT_MIN
    -1048576
    -65536
    -32768
    -1024
    -2
    -1

For every tested value:

    setsockopt -> 0
    getsockopt -> 0
    returned value == requested value

### Negative RCVBUF range

The same behaviour was observed for SO_RCVBUF.

All tested values down to INT_MIN were accepted and returned unchanged.

Therefore the characterized native rule is:

    any tested signed value <= 65535 is accepted

while earlier characterization established:

    values >= 65536 -> EINVAL

### Real TCP send backpressure

The PC peer deliberately stopped reading from an established TCP
connection. The Wii U socket was nonblocking and repeatedly called
send() until nsysnet returned EWOULDBLOCK.

Native results:

    default SNDBUF=8192
        accepted bytes=13040
        sends=9
        terminal error=EWOULDBLOCK
        socketlasterr=6
        SO_TXDATA=8760

    SNDBUF=1
        accepted bytes=2720
        sends=2
        terminal error=EWOULDBLOCK
        socketlasterr=6
        SO_TXDATA=1360

    SNDBUF=4096
        accepted bytes=8560
        sends=6
        SO_TXDATA=4280

    SNDBUF=8192
        accepted bytes=13040
        sends=9
        SO_TXDATA=8760

    SNDBUF=16384
        accepted bytes=21800
        sends=15
        SO_TXDATA=17520

    SNDBUF=65535
        accepted bytes=69980
        sends=48
        SO_TXDATA=65700

In every completed case the socket became writable again after the peer
resumed reading, and all bytes were eventually drained by the peer.

This proves that native SO_SNDBUF is not merely title-visible metadata:
it materially changes the amount of TCP data which can be queued before
a nonblocking send reaches backpressure.

SO_TXDATA also reflects actual queued transmit data on native nsysnet.

### SNDBUF=0 special case

setsockopt(SO_SNDBUF=0) succeeded and getsockopt was not reached because
the subsequent nonblocking TCP connect did not become writable within
the probe timeout:

    errno=116

socketlasterr remained zero because the timeout was generated by the
probe's wait path rather than retrieved as a native socket error.

This case is retained as an observed native result but should be
compared against AX and, if necessary, repeated before assigning exact
socket-level semantics to SNDBUF=0.
