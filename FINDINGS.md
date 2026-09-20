# AX88179 Wii U Aroma Driver — Findings

This document summarizes the current reverse-engineering results, hardware validation and architecture findings for the AX88179 Wii U Aroma driver.

For installation, configuration and general project information, see [README.md](README.md).

---

## Status

### ✅ Validated

* IPv4 / ARP
* DHCP
* ICMP
* TCP client
* TCP `listen` / `accept`
* UDP
* `select`
* Nonblocking sockets
* `errno`, `socketlasterr()` and `SO_ERROR`
* Native + AX socket coexistence
* Native socket descriptor exhaustion behavior
* Socket option translation
* `SO_SNDBUF` ABI and exact TCP backpressure
* `SO_RCVBUF` and TCP `SO_RXDATA`
* UDP `SO_RXDATA`
* Multicast RX and TX
* `IP_MULTICAST_TTL`
* `IP_MULTICAST_LOOP`
* `IP_MULTICAST_IF`
* `IP_ADD_MEMBERSHIP`
* `IP_DROP_MEMBERSHIP`
* `sendto_multi`
* `sendto_multi_ex`
* `recvfrom_multi`
* `recvfrom_ex`
* IPv4 TTL through `recvfrom_ex`
* `gethostbyaddr`
* Async DNS APIs
* `dns_abort_by_hname`
* Native-compatible `clear_resolver_cache` behavior
* Nintendo NSSL transport through AX88179
* Wii U Menu + GAME FunctionPatcher coverage
* Stable Menu -> game -> Menu transitions
* Per-title AX/lwIP/UHS cleanup and recreation
* FTPiiU native-listener handoff to AX
* USB hot-unplug / reconnect
* Ethernet link loss / restoration
* DHCP recovery after interface recreation
* Socket exhaustion recovery
* Buffer exhaustion recovery
* Sustained concurrent TCP/UDP traffic
* Real game networking through Pretendo
* Highest-rate RX burst parity (~147 Mbit/s)

### ⏳ Still open

* DHCP renew/rebind at real lease expiry
* Browser process networking
* HOME Menu / eShop / Download Manager dedicated process coverage
* Root-process support
* System network-state emulation
* Fully Wi-Fi-free operation
* Wider game compatibility testing

---

# Current architecture

The socket shim currently targets:

```text
FP_TARGET_PROCESS_GAME_AND_MENU
```

using **one FunctionPatcher registration per exported function**.

Normal traffic:

```text
Wii U Menu / GAME title
    |
    v
nsysnet API
    |
    v
Aroma FunctionPatcher shim
    |
    v
lwIP
    |
    v
AX88179 driver
    |
    v
Wii U UHS
    |
    v
USB Ethernet
```

Only sockets created by the shim are owned by AX/lwIP.

Foreign/native descriptors remain on the original Wii U network stack.

Each AX socket reserves a real native public descriptor and maps it to a private lwIP descriptor. This prevents descriptor collisions and allows native and AX sockets to coexist.

---

# FunctionPatcher GAME + Menu lifecycle finding

A major transition bug was isolated during Menu -> GAME testing.

The original shim registered every `nsysnet` replacement twice:

```text
replacement #1 -> FP_TARGET_PROCESS_GAME
replacement #2 -> FP_TARGET_PROCESS_WII_U_MENU
```

For example:

```c
SHIM_PATCH_PROCESS(socket, FP_TARGET_PROCESS_GAME);
SHIM_PATCH_PROCESS(socket, FP_TARGET_PROCESS_WII_U_MENU);
```

This appeared reasonable because the replacement function itself was identical.

It is not safe with the FunctionPatcher replacement model.

## Shared `real_*` pointer

A `DECL_FUNCTION` replacement provides one shared original-function pointer:

```text
real_socket
real_connect
real_send
...
```

Each FunctionPatcher registration stores the address of that shared pointer.

Each patch also builds its own trampoline to the original function and updates the shared `real_*` pointer to point at that trampoline.

Registering the same replacement twice therefore produces:

```text
GAME patch --------\
                    > same real_socket storage
MENU patch --------/
```

with two independently generated trampolines.

The second patch can overwrite the `real_socket` pointer expected by the first registration.

## Hardware symptoms

The failure appeared as a Wii U Menu -> game transition hang.

A long bisection produced the following results:

```text
SHIM=0
    -> transition OK

full shim, all routing forced native
    -> transition hangs

socket hooks only
    -> transition hangs

socket + socketclose
    -> transition hangs

socketclose only
    -> transition OK

socket only, registered for GAME + MENU separately
    -> transition hangs

socket only, MENU target only
    -> transition OK

socket only, GAME target only
    -> transition OK

socket only, GAME_AND_MENU combined target
    -> transition OK
```

This isolated the problem to **multiple process-specific registrations of the same replacement**, not to lwIP or to the `socket()` implementation itself.

## Correct solution

FunctionPatcher already provides:

```text
FP_TARGET_PROCESS_GAME_AND_MENU
```

The shim now uses one registration:

```c
SHIM_PATCH_PROCESS(
    socket,
    FP_TARGET_PROCESS_GAME_AND_MENU);
```

and the generic `SHIM_PATCH()` macro does the same for every covered export.

Result:

```text
old:
2 FunctionPatcher registrations per function

new:
1 GAME_AND_MENU registration per function
```

Full configuration:

```text
22 socket exports
 3 NSSL exports
11 DNS exports
----------------
36 handles
```

Previously the equivalent GAME + MENU coverage required 72 handles.

The combined implementation has been validated with the full AX route, AX DNS, NSSL bridge and FTP handoff enabled.

---

# Title transition validation

After fixing the FunctionPatcher registration model, all temporary lifecycle workarounds were removed.

The normal WUT devoptab finalization was restored.

Per-title AX/lwIP/UHS cleanup was also restored.

Validated sequence:

```text
Wii U Menu
    |
    v
Super Mario Maker
    |
    v
Wii U Menu
    |
    v
Super Mario Maker
    |
    v
download a game / content
    |
    v
launch and play
    |
    v
Wii U Menu
```

No transition freeze occurred.

Configuration during this validation:

```ini
[dhcp]
mode=keep_first

[debug]
shim_trace=0

[compat]
dns=ax
route=ax
nssl=bridge
ftp_handoff=on
```

The network worker is stopped at:

```text
WUMS_ALL_APPLICATION_ENDS_DONE
```

This is late enough that all modules have received their normal `APPLICATION_ENDS` notification, while WUT socket/devoptab state still exists.

The worker then retires the title-owned networking state before the next title starts.

---

# Native socket limits

Native title-visible descriptors:

```text
fd 4..31
```

Total:

```text
28 sockets
```

Observed for:

* TCP only
* UDP only
* mixed TCP/UDP

The 29th socket fails with:

```text
EMFILE
socketlasterr() = 51
```

AX reproduces this visible limit through reserved native descriptors.

---

# TCP `SO_SNDBUF`

Native Wii U behavior was characterized with an aligned 128 KiB single-send probe.

Visible `SO_SNDBUF` values remain exactly as requested.

Internal send capacity follows:

```text
effective = ceil(requested / 1360) * 1360
```

Measured parity:

| Requested | Native |    AX |
| --------: | -----: | ----: |
|         1 |   1360 |  1360 |
|      4096 |   5440 |  5440 |
|      8192 |   9520 |  9520 |
|     16384 |  17680 | 17680 |
|     65535 |  66640 | 66640 |

After filling the send capacity, a second nonblocking `send()` returns:

```text
errno = EWOULDBLOCK
socketlasterr() = 6
```

Because `65535` maps to `66640`, lwIP's internal `snd_buf` and `snd_buf_max` had to be widened beyond 16 bits.

---

# TCP `SO_RCVBUF`

Visible native values are reproduced exactly:

| Requested | Native |    AX |
| --------: | -----: | ----: |
|   default |   8192 |  8192 |
|         1 |      1 |     1 |
|      4096 |   4096 |  4096 |
|      8192 |   8192 |  8192 |
|     16384 |  16384 | 16384 |
|     65535 |  65535 | 65535 |

The configured value also controls real TCP receive backpressure.

Draining the socket reopens the receive window correctly.

---

# UDP `SO_RXDATA`

Native Wii U UDP accounting includes:

```text
payload + 16 bytes per queued datagram
```

For 46 queued datagrams of 1400 bytes:

```text
payload       = 46 × 1400 = 64400
metadata      = 46 ×   16 =   736
SO_RXDATA                  = 65136
```

AX reproduces this exactly:

```text
RXDATA=65136 packets=46 bytes=64400
```

With 8 sockets:

```text
46 packets/socket
368 packets total
515200 bytes payload
```

Recovery after draining:

```text
sockets = 8/8
packets = 24/24
```

---

# UDP multi-datagram APIs

Validated:

```text
sendto_multi
sendto_multi_ex
recvfrom_multi
recvfrom_ex
```

Native ABI details discovered during testing include:

* several buffers and structures require `0x40` alignment;
* IOS vector handling can affect unaligned transfers;
* the raw nsysnet timeout structure is:

```c
struct {
    int32_t sec;
    int32_t usec;
};
```

`recvfrom_ex` also exposes the real IPv4 TTL when:

```text
MSG_IP_RECVTTL = 0x40
```

---

# Multicast

Validated over AX88179:

```text
IP_MULTICAST_TTL
IP_MULTICAST_LOOP
IP_MULTICAST_IF
IP_ADD_MEMBERSHIP
IP_DROP_MEMBERSHIP
```

Both real multicast transmission and reception work.

---

# DNS

Implemented and validated through AX/lwIP:

```text
gethostbyname
gethostbyaddr
getaddrinfo
getaddrinfo_rs
getaddrinfo_async
getaddrinfo_async_rs
freeaddrinfo
getnameinfo
get_h_errno
dns_abort_by_hname
gai_strerror
```

## Async DNS

Native asynchronous resolution behaves as polling:

```text
first call:
    EAI_INPROGRESS

later calls:
    EAI_INPROGRESS

when complete:
    0 + valid addrinfo
```

Numeric addresses may complete immediately.

AX reproduces this using lwIP's asynchronous DNS resolver.

## `dns_abort_by_hname`

Native observable behavior:

```text
dns_abort_by_hname(...) -> 0
```

This remains true when:

* no matching request exists;
* the result is already cached;
* the same hostname is pending;
* another hostname is pending.

Pending resolutions continue normally afterward.

AX therefore implements it as a successful no-op while AX DNS is active.

## `clear_resolver_cache`

Native tests showed no observable reason to aggressively flush lwIP DNS state:

* positive cached results remain usable;
* pending queries continue;
* negative lookups do not become meaningfully different afterward.

The native implementation is therefore retained instead of flushing lwIP.

## `gethostbyaddr`

Real IPv4 PTR resolution is implemented.

Examples:

```text
8.8.8.8 -> dns.google
1.1.1.1 -> one.one.one.one
```

Native behavior reproduced:

```text
h_addrtype = AF_INET
h_length   = 4
h_aliases  = { NULL }
h_addr_list = { NULL }
```

`gethostbyaddr()` does not modify `h_errno`.

---

# Nintendo NSSL

Nintendo NSSL requires a real native `nsysnet` descriptor.

Instead of replacing Nintendo TLS, the driver uses a localhost transport bridge:

```text
Nintendo NSSL / IOS-NSEC
        |
        v
native nsysnet socket
        |
        v
127.0.0.1
        |
        v
PPC relay
        |
        v
lwIP socket
        |
        v
AX88179
```

The relay only transports encrypted TLS bytes.

Nintendo still handles:

* TLS
* certificates
* handshake state
* NSSL semantics

Validated with real HTTPS traffic through Pretendo.

The current hook set includes:

```text
NSSLCreateConnection
NSSLDestroyConnection
NSSLFinish
```

---

# FTP handoff

FTPiiU can create its native port-21 listener before the AX shim is active.

Closing another thread's descriptor externally is unsafe, so the AX module does not force-close the listener.

Instead it marks the native listener for restart.

When FTPiiU next polls the descriptor, the shim returns a one-shot network-style failure so FTPiiU follows its existing listener-recreation path.

The new listener is then created while the AX shim is active.

Validated with:

```ini
ftp_handoff=on
```

during Menu/game transition testing.

---

# Real-game validation

## Minecraft: Wii U Edition

Validated:

* GAME sockets over AX/lwIP
* Pretendo connectivity
* map creation
* gameplay

## Super Mario Maker

Validated:

* Course World
* NSSL over AX88179
* level download
* launching downloaded content
* gameplay
* repeated Menu -> game -> Menu transitions

## Super Smash Bros. for Wii U

Validated end-to-end:

* Pretendo access
* Nintendo NSSL
* matchmaking
* connection to a real remote player
* P2P/game traffic
* complete online match

This remains one of the strongest real-world validations of the networking stack.

---

# Driver lifecycle

The network worker is recreated across title transitions.

Current startup guard:

```text
~2 s per title
```

Typical PHY timings:

```text
cold open : ~750 ms
cold link : ~3.1 s

warm open : ~250 ms
warm link : ~13-15 ms
```

`dhcp=keep_first` caches the first successful configuration for the current Aroma session.

The title transition path uses:

```text
APPLICATION_ENDS for all modules
        |
        v
ALL_APPLICATION_ENDS_DONE
        |
        v
stop AX worker
        |
        v
stop/retire per-title lwIP state
        |
        v
abandon title-owned AX/UHS state
        |
        v
normal WUT socket/devoptab finalization
```

The WUT devoptab finalizer is enabled normally.

---

# Recovery

## USB hot-unplug

Validated:

```text
USB unplug
    |
UHS errors
    |
AX/lwIP interface stopped
    |
UHS handle closed
    |
adapter replugged
    |
cold PHY initialization
    |
RX ring recreated
    |
network restored
```

No Wii U reboot is required.

## Ethernet cable

RJ45 link-down and link-up are recovered automatically without reopening the USB device.

## DHCP

A complete DHCP cycle after AX interface recreation has been validated.

Still untested:

```text
real DHCP lease expiry / renew / rebind
```

---

# RX pipeline

The driver uses three asynchronous UHS bulk-IN requests:

```text
RX_ASYNC_SLOTS = 3
```

Each UHS completion signals an auto-reset OSEvent, immediately waking the network worker.

The RX path is:

```text
UHS async completion
    |
    v
OSSignalEvent()
    |
    v
network worker wakes
    |
    v
DMA slot is rearmed
    |
    v
AX aggregate is parsed
    |
    v
frames are submitted to lwIP
```

The previous 2 ms polling delay was identified as the cause of packet loss during high-rate bursts.

After replacing the delay with callback-driven wakeups, the high-rate UDP test reaches full parity:

```text
~147 Mbit/s
368 / 368 packets
515200 bytes
```

Recovery:

```text
8 / 8 sockets
24 / 24 packets
```

The same full result is reproduced across all three tested send rates.

High-rate RX burst parity is therefore validated.

---

# Current lwIP configuration

Important values:

```text
PBUF_POOL_SIZE             = 512
MEMP_NUM_NETBUF            = 512
MEMP_NUM_NETCONN           = 32
MEMP_NUM_TCPIP_MSG_INPKT   = 512

TCPIP_MBOX_SIZE            = 512

DEFAULT_TCP_RECVMBOX_SIZE  = 64
DEFAULT_UDP_RECVMBOX_SIZE  = 64

TCP_WND                    = 65535

LWIP_TCPIP_CORE_LOCKING       = 1
LWIP_TCPIP_CORE_LOCKING_INPUT = 0

TCPIP_THREAD_PRIO          = 1
```

Coreinit mapping:

```text
lwIP tcpip thread -> priority 5
AX worker         -> priority 16
```

A lower tcpip priority was tested and caused a significant RX regression.

---

# Debugging rules

Avoid high-frequency logging inside socket and NSSL hooks.

Logging can:

* change timing;
* alter `socketlasterr()` observations;
* disturb NEX/game networking.

Normal compatibility testing should use:

```ini
[debug]
shim_trace=0
```

Prefer counters, deferred traces and worker-side logging.

---

# Remaining roadmap

## 1. DHCP lifecycle

Validate a genuine:

```text
BOUND -> RENEWING -> REBINDING
```

cycle on real hardware.

## 2. System process coverage

Current:

```text
GAME + Wii U Menu
```

through:

```text
FP_TARGET_PROCESS_GAME_AND_MENU
```

Next targets:

```text
Browser
HOME Menu
eShop
Download Manager
root/system processes
```

## 3. System network state

Eventually expose the AX interface through the higher-level Wii U network configuration/state APIs so the system can report:

```text
link    = UP
IPv4    = AX DHCP address
gateway = AX DHCP gateway
DNS     = AX DHCP DNS
```

## 4. Wi-Fi-free operation

The final goal is normal Wii U networking through AX88179 without applications or system software needing to know that a custom USB Ethernet driver is present.
