#!/bin/bash
# Script de déploiement du module Aroma vers la Wii U
# Upload via FTP (192.168.2.124:21)

set -e

WIIU_IP="192.168.2.124"
MODULE_SRC="/home/wozt/dev/capture2cloud/wiiu_ethernet/aroma_module/AX88179Module.wms"
WIIU_DEST="/vol/internal/user/aroma/modules/AX88179Module.wms"

echo "=== Déploiement AX88179 Module vers Wii U ==="
echo "Module source: $MODULE_SRC"
echo "Cible Wii U: $WIIU_DEST"

# Vérifier que le module existe
if [ ! -f "$MODULE_SRC" ]; then
    echo "Erreur: Module non trouvé: $MODULE_SRC"
    echo "Compil d'abord avec: cd aroma_module && make SHIM=1"
    exit 1
fi

# Upload via FTP
echo "Upload via FTP anonyme..."
ftp -inv "$WIIU_IP" <<EOF
anonymous
@
binary
put "$MODULE_SRC" "$WIIU_DEST"
bye
EOF

echo ""
echo "=== Module déployé avec succès ==="
echo ""
echo "Pour tester:"
echo "1. Redémarrer la Wii U"
echo "2. Lancer Aroma"
echo "3. Lancer un homebrew (test_shim_boot, etc.)"
echo ""
echo "Vérifier les logs:"
echo "  - Écran TV: état réseau, IP, tests"
echo "  - PC: nc -ul 18880 pour les logs UDP"
echo ""
