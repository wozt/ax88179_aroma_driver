#!/bin/bash
set -e

WIIU_IP="192.168.2.190"
RPX="$(dirname "$0")/poweroff.rpx"

echo "Extinction de la Wii U ($WIIU_IP)..."
WIILOAD=tcp:$WIIU_IP wiiload "$RPX"
