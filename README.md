# AX88179 Aroma Driver

Experimental **ASIX AX88179 USB Ethernet driver for the Nintendo Wii U**, implemented as an Aroma/WUMS background module.

The project provides:

* a native user-mode AX88179 USB driver;
* a dedicated lwIP TCP/IP stack;
* an Aroma `nsysnet.rpl` compatibility shim;
* Nintendo NSSL/TLS transport through the AX88179.

> [!IMPORTANT]
> This project is still experimental.
>
> Networking for Wii U games and homebrew running as the **GAME process** is already functional and has been validated with real online games.
>
> It is **not yet a complete system-wide Ethernet replacement**. Wii U system applications such as the Menu, Browser and eShop are the next major integration target.

---

## Current status

### ✅ Working and hardware-tested

* AX88179 USB detection and initialization
* MAC address retrieval
* PHY initialization
* Ethernet RX / TX
* Asynchronous USB RX
* IPv4 / ARP
* DHCP
* ICMP / ping
* TCP
* UDP
* TCP server sockets
* Nonblocking sockets
* `select`
* Native + AX socket coexistence
* Socket option compatibility
* TCP send/receive backpressure
* UDP receive-buffer accounting
* Multicast
* Wii U multi-datagram socket APIs
* Synchronous DNS
* Asynchronous DNS
* Reverse IPv4 DNS
* Nintendo NSSL/TLS over AX88179
* USB hot-unplug/replug recovery
* Ethernet link loss/recovery
* DHCP recovery after interface recreation
* Prolonged concurrent TCP/UDP traffic

### 🎮 Real-game validation

* Minecraft: Wii U Edition + Pretendo
* Super Mario Maker + Pretendo
* Super Smash Bros. for Wii U + Pretendo
* Real Smash matchmaking and complete online match

### 🚧 Still in development

* Wii U Menu networking
* Browser networking
* HOME Menu / eShop / Download Manager networking
* Root/system process support
* Wi-Fi-free system operation
* DHCP renew/rebind validation
* Wider game compatibility

---

## Hardware support

The current driver explicitly matches:

```text
VID:PID = 0b95:1790
ASIX AX88179
```

Other ASIX USB Ethernet controllers should not be assumed compatible unless explicitly added and tested.

---

## Architecture

### Normal sockets

```text
Wii U game / homebrew
        |
        | nsysnet API
        v
+----------------------+
| Aroma nsysnet shim   |
| FunctionPatcher      |
+----------+-----------+
           |
           v
+----------------------+
| lwIP TCP/IP stack    |
+----------+-----------+
           |
           v
+----------------------+
| AX88179 driver       |
+----------+-----------+
           |
           v
       Wii U UHS
           |
           v
      USB Ethernet
```

The shim currently targets:

```text
FP_TARGET_PROCESS_GAME
```

Only sockets created by the shim are routed through AX/lwIP.

Existing native sockets remain native.

---

## Nintendo NSSL / TLS

Nintendo NSSL cannot directly consume a private lwIP descriptor.

The project solves this without replacing Nintendo's TLS stack.

```text
Game
 |
 v
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
 |
 v
Internet / Pretendo
```

Nintendo still owns:

* TLS
* certificates
* handshakes
* NSSL state

The relay only transports the encrypted byte stream.

This path is working in real games.

---

## Repository structure

```text
.
├── aroma_module/       WUMS module and nsysnet/NSSL shim
├── driver/             AX88179 USB driver
├── net/                lwIP integration
│   └── port/           Wii U/coreinit lwIP port
├── common/             Shared probe/helper code
├── tests/              Host-side tests
├── tools/              Wii U probes and development tools
├── vendor/             Vendored dependencies
├── FINDINGS.md         Reverse-engineering and validation notes
└── README.md
```

---

## Requirements

A Wii U running **Aroma** is required.

Build dependencies include:

* devkitPro
* devkitPPC
* WUT
* WUMS
* libmocha
* libfunctionpatcher
* lwIP

Example environment:

```bash
export DEVKITPRO=/opt/devkitpro
```

---

## Building

Build the normal module with the `nsysnet` shim enabled:

```bash
cd aroma_module
make clean
make SHIM=1 -j"$(nproc)"
```

Result:

```text
AX88179Module.wms
```

To build only the Ethernet/lwIP module without socket interception:

```bash
cd aroma_module
make clean
make SHIM=0 -j"$(nproc)"
```

---

## Installation

Copy:

```text
AX88179Module.wms
```

to:

```text
SD:/wiiu/environments/aroma/modules/AX88179Module.wms
```

Then reboot into Aroma.

The FunctionPatcher Aroma module is required when the `nsysnet` shim is enabled.

---

## Configuration

Configuration file:

```text
SD:/wiiu/ax88179/config.ini
```

Recommended development configuration:

```ini
[dhcp]
mode=keep_first

[debug]
shim_trace=0

[compat]
dns=ax
route=ax
nssl=bridge
```

### DHCP

```ini
mode=keep_first
```

Available modes:

* `keep_first` — keep the first successful IPv4 configuration for the current Aroma session and reuse it across title transitions.
* `always` — perform DHCP again when the AX network interface is recreated.

A full console reboot clears the cached session configuration.

---

### Route selection

```ini
route=ax
```

Values:

* `ax` — route compatible GAME-process sockets through lwIP/AX88179.
* `native` — diagnostic mode using the original Wii U network stack.

---

### DNS

```ini
dns=ax
```

Values:

* `ax` — use the AX/lwIP resolver implementation.
* `system` — leave DNS resolution on the native Wii U resolver.

AX mode supports the DNS APIs currently required by the tested titles, including asynchronous resolution.

The current Pretendo hostname rewrite logic is development-oriented and does not yet provide a generic replacement for every possible custom Inkay configuration.

---

### NSSL

```ini
nssl=bridge
```

Values:

* `bridge` — Nintendo NSSL uses the localhost relay and communicates with the Internet through AX/lwIP.
* `native` — legacy fallback using the original native network path.

---

### Debugging

```ini
shim_trace=0
```

Levels:

* `0` — normal mode
* `1` — general socket tracing
* `2` — verbose tracing

For real games, `0` is recommended.

High-frequency logging can change timing and interfere with NEX or other networking behavior.

---

## Socket compatibility

The shim translates between Wii U `nsysnet` behavior and lwIP.

Implemented areas include:

```text
socket
socketclose
socketclose_all

bind
connect
listen
accept
shutdown

send
sendto
sendto_multi
sendto_multi_ex

recv
recvfrom
recvfrom_ex
recvfrom_multi

select

setsockopt
getsockopt

getsockname
getpeername

socketlasterr
```

DNS support includes:

```text
gethostbyname
gethostbyaddr

getaddrinfo
getaddrinfo_rs
getaddrinfo_async
getaddrinfo_async_rs

dns_abort_by_hname

freeaddrinfo
getnameinfo
get_h_errno
gai_strerror
```

Nintendo NSSL integration is handled through:

```text
NSSLCreateConnection
```

---

## Native descriptor compatibility

The native Wii U title socket range observed on hardware is:

```text
4..31
```

That gives:

```text
28 simultaneous public sockets
```

The AX shim preserves this behavior.

Each AX-backed socket reserves a native public descriptor and maps it to an internal lwIP descriptor.

This allows:

```text
native sockets + AX sockets
```

to coexist safely inside the same title.

---

## TCP backpressure parity

Native Wii U TCP send capacity does not directly equal the visible `SO_SNDBUF` value.

Measured behavior:

```text
effective = ceil(SO_SNDBUF / 1360) * 1360
```

| `SO_SNDBUF` | Native capacity | AX capacity |
| ----------: | --------------: | ----------: |
|           1 |            1360 |        1360 |
|        4096 |            5440 |        5440 |
|        8192 |            9520 |        9520 |
|       16384 |           17680 |       17680 |
|       65535 |           66640 |       66640 |

AX now reproduces the native backpressure behavior exactly for these measured cases.

---

## UDP receive accounting

Native Wii U `SO_RXDATA` counts:

```text
payload + 16 bytes per queued UDP datagram
```

Example:

```text
46 datagrams × 1400 payload bytes = 64400
46 datagrams ×   16 metadata bytes = 736

SO_RXDATA = 65136
```

AX reproduces this accounting.

---

## Multicast

Validated:

```text
IP_MULTICAST_TTL
IP_MULTICAST_LOOP
IP_MULTICAST_IF
IP_ADD_MEMBERSHIP
IP_DROP_MEMBERSHIP
```

Both real multicast transmission and reception have been tested.

---

## DNS

The AX resolver supports both synchronous and asynchronous Wii U DNS behavior.

Native asynchronous behavior is polling-based:

```text
first call  -> EAI_INPROGRESS
later calls -> EAI_INPROGRESS
complete    -> 0 + addrinfo
```

AX reproduces this behavior using lwIP's asynchronous DNS system.

Reverse IPv4 DNS is also implemented.

Examples validated:

```text
8.8.8.8 -> dns.google
1.1.1.1 -> one.one.one.one
```

---

## Recovery

### USB hot-unplug

The AX88179 can be unplugged and reconnected while Aroma remains running.

The driver automatically:

```text
detects UHS failure
stops the old interface
closes the old UHS handle
waits for the adapter
performs a cold PHY initialization
recreates the RX ring
restores networking
```

No console reboot is required.

### Ethernet cable

RJ45 link loss and restoration are handled without reopening the USB device.

### DHCP

DHCP has been validated after complete interface recreation.

A real lease-expiration renew/rebind sequence remains to be tested.

---

## Title lifecycle

The AX worker is recreated across title transitions.

Typical startup guards:

```text
first Aroma worker : ~25 s
later workers      : ~2 s
```

Typical hardware timings:

```text
cold open : ~750 ms
cold link : ~3.1 s

warm open : ~250 ms
warm link : ~13-15 ms
```

Warm reopen is used when the existing PHY state is still valid.

Otherwise the driver falls back to a cold reset and autonegotiation.

---

## Real-game tests

### Minecraft: Wii U Edition

Validated through Pretendo:

* AX/lwIP sockets
* network connection
* map creation
* gameplay

### Super Mario Maker

Validated:

* Course World
* Pretendo
* NSSL through AX88179
* level download
* gameplay

### Super Smash Bros. for Wii U

Validated:

* online service access
* NSSL through AX88179
* matchmaking
* real remote player
* online gameplay
* complete match

---

## Current limitations

The project currently redirects only the GAME process.

The following are not yet fully routed through AX88179:

```text
Wii U Menu
Browser
HOME Menu
eShop
Download Manager
root process
other system services
```

Higher-level Wii U network-state APIs also still report the original system networking state.

The final system-wide implementation will need to expose the AX state as something equivalent to:

```text
link    = UP
IPv4    = AX DHCP address
gateway = AX DHCP gateway
DNS     = AX DHCP DNS
```

---

## Roadmap

### 1. Finish RX performance

- [x] Full UDP RX burst capacity at ~147 Mbit/s payload is done

### 2. Validate DHCP renew/rebind

Test a genuine lease lifecycle:

```text
BOUND
  -> RENEWING
  -> REBINDING
```

### 3. Expand process coverage

Planned order:

```text
GAME
  -> Wii U Menu
  -> Browser
  -> HOME Menu / eShop / Download Manager
  -> root/system processes
```

### 4. Replace system network state

Make the rest of the Wii U recognize the AX interface as its active network connection.

### 5. Remove the Wi-Fi dependency

The long-term goal is:

> Plug an AX88179 USB Ethernet adapter into a Wii U and use it as the console's normal network interface without applications needing to know that a custom driver is present.

---

## Development notes

Detailed reverse-engineering results and native-vs-AX measurements are maintained in:

[FINDINGS.md](FINDINGS.md)

---

## Warning

This software performs low-level USB operations and applies a volatile in-memory IOSU patch.

It is experimental software intended for Wii U homebrew development and reverse engineering.

Use it at your own risk.

---

## License

No project license has been selected yet.

The licensing requirements of the original project code and vendored dependencies should be documented before distributing official releases or accepting external contributions.
