# AX88179 Aroma Driver

Experimental **AX88179 / AX88178A USB Ethernet driver for the Nintendo Wii U**, designed to run as an [Aroma](https://aroma.foryour.cafe/) background module.

The project provides a native AX88179 USB driver, a dedicated **lwIP network stack**, and an experimental `nsysnet` socket shim allowing Wii U homebrew and titles to communicate through the USB Ethernet adapter.

> [!IMPORTANT]
> This project is still experimental.
>
> Basic Ethernet networking is functional, including DHCP, ICMP, TCP and UDP. Transparent system-wide Wii U networking is **not implemented yet**.

---

## Current status

### Working

* [x] AX88179 USB detection and initialization
* [x] MAC address retrieval
* [x] Ethernet link detection
* [x] Ethernet RX
* [x] Ethernet TX
* [x] lwIP integration
* [x] DHCP
* [x] ICMP / ping
* [x] TCP
* [x] UDP
* [x] Experimental `nsysnet` socket interception
* [x] TCP/UDP traffic from a Wii U homebrew routed through the AX88179 interface
* [x]Configurable DHCP behavior through `SD:/wiiu/ax88179/config.ini`.
* [x]Optional session-level DHCP lease caching for faster network recovery after title transitions.
* [x]Network worker stop/restart across title transitions while retaining the first successful DHCP configuration for the current Aroma session.


### Experimental / incomplete

* [ ] Reliable networking across every title transition
* [ ] Broad Wii U game compatibility
* [ ] Transparent NSSL/TLS support
* [ ] Wii U system services
* [ ] Wii U Menu networking
* [ ] Browser / eShop networking
* [ ] Fully transparent system-wide Ethernet replacement

---

## Architecture

```text
                 Wii U application / game
                           │
                           │ nsysnet API
                           ▼
                  ┌──────────────────┐
                  │  nsysnet shim    │
                  │ FunctionPatcher  │
                  └────────┬─────────┘
                           │
                           ▼
                     ┌──────────┐
                     │   lwIP   │
                     │ TCP/IP   │
                     └────┬─────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ AX88179 driver  │
                 └────────┬────────┘
                          │
                          ▼
                    Wii U UHS API
                          │
                          ▼
                     USB Ethernet
                          │
                          ▼
                 AX88179 / AX88178A
```

The Aroma module applies the required IOSU endpoint ownership patch, initializes the USB Ethernet adapter, starts its own lwIP stack and can optionally intercept `nsysnet.rpl` socket functions.

The AX88179 driver and lwIP stack are independent from the Wii U's native network interface.

---

## Repository structure

```text
.
├── aroma_module/       Aroma/WUMS module and nsysnet shim
├── driver/             AX88179 USB Ethernet driver
├── net/                lwIP ↔ AX88179 integration
│   └── port/           Wii U/coreinit lwIP port
├── common/             Shared Wii U helper code
├── tests/              Host and Wii U tests
├── vendor/             Vendored dependencies
├── FINDINGS.md         Development notes and reverse-engineering findings
└── README.md
```

The main implementation lives in:

```text
driver/
net/
aroma_module/
```

Diagnostic programs and historical bring-up tests are kept separately from the production module.

---

## Requirements

A Wii U running **Aroma** is required.

For building, you need a Wii U homebrew development environment including:

* devkitPro
* devkitPPC
* WUT
* WUMS
* libmocha
* libfunctionpatcher
* lwIP

The project expects `DEVKITPRO` to be configured in your environment.

For example:

```bash
export DEVKITPRO=/opt/devkitpro
```

Some dependencies are expected under:

```text
vendor/
├── wums/
├── libmocha/
├── functionpatcher/
└── lwip/
```

---

## Building

Build the Aroma module with:

```bash
cd aroma_module
make
```

The default configuration enables the experimental `nsysnet` shim.

Explicitly build it with:

```bash
make SHIM=1
```

To build the Ethernet driver/lwIP module without socket interception:

```bash
make SHIM=0
```

Clean the build with:

```bash
make clean
```

The resulting Aroma module is:

```text
AX88179Module.wms
```

---

## Installation

Copy:

```text
AX88179Module.wms
```

to:

```text
sd:/wiiu/environments/aroma/modules/
```

Then reboot the Wii U into Aroma.

The module is loaded automatically by the environment; no application needs to be launched manually.

When socket interception is enabled, Aroma's **FunctionPatcher module** must also be installed.

---

## Configuration

The module configuration file is located at:

```text
SD:/wiiu/ax88179/config.ini
```

DHCP behavior can be configured with:

```ini
[dhcp]
mode=keep_first
```

Available modes:

* `keep_first` — perform DHCP once, then reuse the first successful IPv4 configuration across title transitions for the remainder of the Aroma session.
* `always` — perform a new DHCP negotiation after every title transition.

`keep_first` only caches the configuration in RAM. A full console reboot clears it and causes DHCP to run normally again.

---

## Debugging

The module outputs diagnostic messages through the Wii U logging facilities.

They can be monitored from a computer using `udplogserver`.

A successful initialization should eventually show the AX88179 interface obtaining a DHCP lease.

The project also contains dedicated diagnostic applications and host-side regression tests for testing the driver, lwIP integration and socket shim independently.

---

## Verified networking

The AX88179 path has successfully been tested with:

```text
USB initialization
        ↓
Link detection
        ↓
Ethernet RX/TX
        ↓
lwIP
        ↓
DHCP
        ↓
IPv4 address
        ↓
ICMP
        ↓
TCP + UDP
```

TCP and UDP echo tests have successfully passed with payload sizes including:

```text
32 bytes
504 bytes
1024 bytes
1400 bytes
```

Traffic capture confirmed that the packets originated from the **AX88179 Ethernet interface** rather than the Wii U's native Wi-Fi interface.

---

## nsysnet shim

The experimental shim intercepts selected exports from:

```text
nsysnet.rpl
```

using Aroma's FunctionPatcher.

Supported functionality currently includes parts of:

* `socket`
* `bind`
* `connect`
* `listen`
* `accept`
* `send`
* `recv`
* `sendto`
* `recvfrom`
* `select`
* socket options
* synchronous DNS resolution

The shim translates between the Wii U `nsysnet` ABI and lwIP.

It also maintains mappings between public Wii U socket descriptors and lwIP sockets so native and Ethernet sockets can coexist.

This part of the project is still experimental.

---

## NSSL / TLS limitation

One of the largest remaining limitations is **NSSL**.

Wii U NSSL expects a native system socket descriptor handled by the original networking stack. A socket created by the AX88179 lwIP shim therefore cannot currently be transparently passed to NSSL.

As a consequence, applications relying on Nintendo's native TLS stack cannot yet transparently use the AX88179 interface.

Solving this is required before the driver can behave like a completely transparent Wii U system Ethernet interface.

---

## Project goal

The long-term goal is:

> Plug an AX88179-based USB Ethernet adapter into a Wii U and use it as a normal network interface without applications needing to know that a custom driver is present.

The project is **not there yet**.

At the moment, the hardware driver and basic IPv4 networking are functional, while transparent integration with the complete Wii U networking environment remains under development.

---

## Development notes

Detailed reverse-engineering results, experiments, known issues and previous approaches are documented in:

**[FINDINGS.md](FINDINGS.md)**

This file is intentionally more verbose than the README and acts as the project's development journal.

---

## Warning

This software performs low-level interaction with Wii U USB services and applies an in-memory IOSU patch.

It is experimental software intended for development and reverse-engineering environments.

Use it at your own risk.

---

## License

No license has been specified yet.

Before distributing binaries or accepting external contributions, a license should be selected for the original project code and the licensing requirements of vendored dependencies should be documented separately.
