#!/usr/bin/env python3
"""Decrypt a Wii U ancast image with the key from this console's OTP.

    ./tools/ancast.py fw.img otp.bin fw.dec

The key is at OTP offset 0x090 -- found, not looked up: every 16-byte
window of the OTP was tried and the one that turns the body from noise
(entropy 7.997 bits/byte) into structure (6.04) is unambiguous. The
image's own SHA-1 cannot confirm it, because that hash covers the body
as STORED, encrypted -- checked, it matches the ciphertext exactly.

The key never appears in this file, and neither the OTP nor any
decrypted image is committed: the OTP is the console's own secret and
the firmware is Nintendo's.
"""
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ANCAST_MAGIC = 0xEFA282D9
OTP_ANCAST_KEY_OFFSET = 0x090
BODY_OFFSET = 0x200


def main(image_path, otp_path, out_path):
    image = open(image_path, "rb").read()
    magic, = struct.unpack(">I", image[:4])
    if magic != ANCAST_MAGIC:
        sys.exit(f"{image_path}: not an ancast image (magic {magic:08X})")

    device, = struct.unpack(">I", image[0x1A8:0x1AC])
    body_size, = struct.unpack(">I", image[0x1AC:0x1B0])
    stored_hash = image[0x1B0:0x1C4]
    body = image[BODY_OFFSET:BODY_OFFSET + body_size]

    print(f"device type {device} ({'Starbuck/ARM' if device == 2 else 'Espresso/PPC'})")
    print(f"body {body_size} bytes, stored sha1 {stored_hash.hex()}")

    key = open(otp_path, "rb").read()[OTP_ANCAST_KEY_OFFSET:OTP_ANCAST_KEY_OFFSET + 16]
    if len(key) != 16 or key == b"\x00" * 16:
        sys.exit(f"{otp_path}: no key at offset 0x{OTP_ANCAST_KEY_OFFSET:03X}")

    # IV zero. CBC resynchronises after one block, so at worst the first
    # sixteen bytes are wrong and everything after them is right.
    dec = Cipher(algorithms.AES(key), modes.CBC(b"\x00" * 16)).decryptor()
    plain = dec.update(body) + dec.finalize()
    open(out_path, "wb").write(plain)
    print(f"wrote {out_path}, {len(plain)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
