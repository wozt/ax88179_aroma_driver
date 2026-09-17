#!/bin/bash

SESSION="wiiu-log"
LOG="/tmp/wiiu.log"

pkill -x udplogserver 2>/dev/null || true
byobu kill-session -t "$SESSION" 2>/dev/null || true

: > "$LOG"

byobu new-session -d -s "$SESSION" \
    "udplogserver 2>&1 | tee -a '$LOG'"

echo "Listener lancé"
echo "Log : $LOG"
echo "Connexion : byobu attach -t $SESSION"