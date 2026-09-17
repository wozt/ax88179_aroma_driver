AX88179 Aroma Driver
====================

Goal
----
AX88179/AX88178A USB Ethernet support for Wii U under Aroma.

Current status
--------------
✓ AX88179 USB initialization
✓ Link detection
✓ Ethernet RX/TX
✓ lwIP
✓ DHCP
✓ ICMP/ping
✓ TCP
✓ UDP
✓ experimental nsysnet socket shim
△ title transitions
△ broader game compatibility
✗ transparent NSSL/TLS
✗ system-wide networking

Architecture
------------
Wii U application
       ↓
   nsysnet shim
       ↓
      lwIP
       ↓
 AX88179 driver
       ↓
      UHS
       ↓
 USB AX88179
