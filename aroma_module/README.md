# AX88179 background module for Aroma
/!\ This document is not up to date 

## Current diagnostic build (0.1.5)

SHIM=1 now exposes explicit `AXShimBeginProbe` / `AXShimEndProbe` module
exports. It installs no hooks on boot. `shim_probe` activates hooks after
its UI and DHCP wait, and only its enrolled thread creates lwIP sockets.
The module keeps registrations resident; per-title ownership is reset
separately. This is a diagnostic scope, not full console networking.
EndProbe requires all owned sockets and DNS lists to have been released.

Builds and host tests pass. Console boot, TCP/UDP echo over the AX IP,
the first HOME return and a second launch passed (2026-09-16). The user
then reported a freeze on the second HOME with the old probe. The probe
now makes HOME request menu exit directly; two consecutive HOME exits
were subsequently confirmed in logs and by the user. This does not
validate the system HOME overlay. See FINDINGS.md. Do not treat the old GAME-only
boot registration as safe: the environment launch also uses UPID 15.


A WUMS module that runs from boot, applies the IOSU endpoint-ownership
patch itself, opens the AX88179 USB Ethernet adapter, brings up its own
threaded lwIP stack (DHCP, TCP, UDP, DNS), and optionally intercepts `nsysnet.rpl` socket exports (`SHIM=1`).
The socket shim is experimental; the default build has it disabled.

## Why a module, not an app

The endpoint patch is a volatile write to IOSU kernel memory: it is gone
at the next reboot. A background module reapplies it every boot, before
it touches the adapter, so nothing has to be launched by hand. That is
the whole reason this is a `.wms` and not the standalone `.rpx` in
`../wiiu_patch_iosu` (which still exists, for testing the patch in
isolation).

## Architecture

- `iosu_patch.c` — NOPs the endpoint-ownership `beq` in IOSU (via
  libmocha), every boot, before the adapter is opened.
- `main.c` — a worker thread (per title process) owns UHS and the
  adapter exclusively.
- `../net/ax_net.c` — lwIP glue. The lwIP `tcpip` thread owns the stack
  core mutex; RX executes synchronously under that mutex on the worker.
  TX uses a slot queue, so only the worker talks to the USB driver.
- `../net/port/sys_arch.c` — lwIP `NO_SYS=0` port on coreinit
  primitives (threads, semaphores, mutexes, message-queue mailboxes).
- `nsysnet_shim.c` — replaces `nsysnet.rpl` exports (sockets, select,
  sockopts, DNS) through the Aroma FunctionPatcher module for games and
  homebrew (`FP_TARGET_PROCESS_GAME`); root and menu are excluded (they mix
  nsysnet internals we cannot cover). It translates the nsysnet ABI (no
  `sa_len`, 16-bit family, different `MSG_*`/`SO_*`/`EAI_*` constants)
  to lwIP. Each Ethernet socket reserves a native descriptor to prevent
  collisions with existing native sockets. A mapping translates public
  descriptors into lwIP descriptors, including in mixed `select` calls.

## What it does, and does not, do

- **Implements:** patch IOSU, acquire the adapter, DHCP a lease, and
  optionally redirect socket calls (`socket`…`select`, `setsockopt`, sync DNS:
  `gethostbyname`, `getaddrinfo`, `getnameinfo`) to titles over USB
  Ethernet.
- **Limitations:**
  - TLS. `NSSLCreateConnection` wraps a *system* fd handled inside IOSU;
    a shim socket cannot be used there. There is no automatic migration
    or transparent TLS fallback for sockets created on Ethernet.
  - `sendto_multi(_ex)`, `recvfrom_ex/_multi`, `getaddrinfo_async(_rs)`,
    `gethostbyaddr`, `netconf_*` — unpatched, they use the original
    nsysnet and must only receive native sockets.
  - The system menu's own services, the eShop and the browser (they are
    separate target processes and rely on NSSL).

## Building

Needs WUMS, libmocha and libfunctionpatcher, vendored under `../vendor`
(see `../vendor/README.md`); with those present:

    make SHIM=0     # driver + DHCP/ping, no socket interception (default)
    make SHIM=1     # experimental socket interception

Both produce `AX88179Module.wms`. Changing SHIM rebuilds the objects.

## Installing

Copy `AX88179Module.wms` to
`sd:/wiiu/environments/aroma/modules/` and reboot. It loads with the
environment; there is nothing to launch. The FunctionPatcher module
(`FunctionPatcherModule.wms`) must be present for title interception;
without it the adapter only serves the module itself.

Watch it with `udplogserver` on the PC -- every line is prefixed
`AX88179`. On a good boot you will see the patch applied, the shim
registrations (SHIM=1), then `DHCP BOUND <ip>, ICMP ready`.

## Socket smoke test

`../shim_probe` is a normal WUT RPX, with no driver or lwIP linked in.
Start its `echo_server.py` on 192.168.2.100 and launch the RPX through
wiiload. It waits 25 seconds, then tests nonblocking TCP and UDP echo
with 32, 504, 1024 and 1400 byte payloads. Check both `AXPROBE` UDP logs
and the peer address recorded by the echo server: 192.168.2.190 proves
Ethernet use; 192.168.2.124 indicates native Wi-Fi.

Host helper regression tests: `python3 ../tests/test_shim_routing.py`.
The errno conversion follows [WUT’s native error map](https://github.com/devkitPro/wut/blob/master/libraries/wutsocket/wut_socket_common.c).
