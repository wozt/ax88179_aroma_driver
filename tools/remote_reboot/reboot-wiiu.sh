#!/bin/bash
set -e

WIIU_IP="192.168.2.124"
RPX="$(dirname "$0")/reboot.rpx"

echo "Envoi du reboot à la Wii U ($WIIU_IP)..."
WIILOAD=tcp:$WIIU_IP wiiload "$RPX"
