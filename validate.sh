#!/bin/bash
# Script de validation des modifications shim boot
# Vérifie que toutes les modifications sont en place

set -e

echo "=== Validation AX88179 Shim Boot ==="
echo ""

# Vérifier FINDINGS.md
echo "[1/6] Vérification FINDINGS.md..."
if grep -q "SHIM AUTO" FINDINGS.md && \
   grep -q "Gel visuel" FINDINGS.md; then
    echo "✓ FINDINGS.md à jour"
else
    echo "✗ FINDINGS.md incomplet"
    exit 1
fi

# Vérifier aroma_module/main.c
echo "[2/6] Vérification aroma_module/main.c..."
if grep -q "AX_DISABLE_SHIM 0" aroma_module/main.c && \
   grep -q "nsysnet_shim_install()" aroma_module/main.c && \
   grep -q "SHIM_INSTALLED" aroma_module/main.c; then
    echo "✓ aroma_module/main.c configure SHIM"
else
    echo "✗ aroma_module/main.c incomplet"
    exit 1
fi

# Vérifier aroma_module/Makefile
echo "[3/6] Vérification aroma_module/Makefile..."
if grep -q "SHIM ?= 1" aroma_module/Makefile; then
    echo "✓ Makefile SHIM=1 par défaut"
else
    echo "✗ Makefile SHIM=0"
    exit 1
fi

# Vérifier debug_progress.h
echo "[4/6] Vérification debug_progress.h..."
if grep -q "AX_MARK_SHIM_INSTALLED" aroma_module/debug_progress.h && \
   grep -q "AX_MARK_NET_STOP" aroma_module/debug_progress.h && \
   grep -q "AX_MARK_ADAPTER_CLOSED" aroma_module/debug_progress.h; then
    echo "✓ debug_progress.h marks ajoutés"
else
    echo "✗ debug_progress.h incomplet"
    exit 1
fi

# Vérifier nsysnet_shim.c/h
echo "[5/6] Vérification nsysnet_shim.c/h..."
if grep -q "handle_count" aroma_module/nsysnet_shim.h && \
   grep -q "handle_count" aroma_module/nsysnet_shim.c; then
    echo "✓ nsysnet_shim handle_count déclaré"
else
    echo "✗ nsysnet_shim incomplet"
    exit 1
fi

# Vérifier ax_net.c/h
echo "[6/6] Vérification ax_net.c/h..."
if grep -q "ax_display_check_exit" net/ax_net.c && \
   grep -q "ax_display_check_exit" net/ax_net.h; then
    echo "✓ ax_display_check_exit implémenté"
else
    echo "✗ ax_display_check_exit incomplet"
    exit 1
fi

echo ""
echo "=== Tous les checks sont valides ==="
echo ""
echo "Prochaines étapes:"
echo "1. Compiler: cd aroma_module && make SHIM=1"
echo "2. Déployer: test_shim_boot/deploy.sh"
echo "3. Tester: Redémarrer Wii U + lancer un homebrew"
echo ""
