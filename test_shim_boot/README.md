# Test shim boot - Validation du shim au démarrage

Un petit homebrew pour valider que le shim fonctionne correctement au démarrage avec le module Aroma.

## Fonctionnalités

- **Affichage à l'écran** (TV/GamePad) avec:
  - État du réseau (IP, TCP/UDP status)
  - Compteurs RX/TX/erreurs
  - Instructions pour quitter (HOME)

- **Tests réseau**:
  - TCP echo (32, 504, 1024, 1400 bytes)
  - UDP echo (32, 504, 1024, 1400 bytes)
  
- **Logs UDP** vers PC (192.168.2.100:18880)

- **SORTIE HOME/MINUS** pour quitter proprement

## Prérequis

1. **Module Aroma avec SHIM=1** compilé et installé
   ```bash
   cd wiiu_ethernet/aroma_module
   make SHIM=1
   # Copier AX88179Module.wms sur sd:/wiiu/environments/aroma/modules/
   ```

2. **Serveur d'écho sur PC** (pour tester TCP/UDP echo)
   ```bash
   cd wiiu_ethernet/test_shim_boot
   python3 echo_server.py
   ```
   
   Le serveur écoute sur `0.0.0.0:18879` pour TCP et UDP.

3. **Réseau**
   - Wii U doit avoir une IP via DHCP (ex: 192.168.2.190)
   - PC doit être sur le même subnet (ex: 192.168.2.100)

## Compilation

```bash
cd wiiu_ethernet/test_shim_boot
make
```

Produit: `test_shim_boot.elf` → `test_shim_boot.wuhb`

## Installation

1. Copier `test_shim_boot.wuhb` sur la SD de la Wii U
   ```bash
   # Via FTP (192.168.2.124:21) ou SD card
   sd:/wiiu/apps/test_shim_boot/test_shim_boot.wuhb
   ```

2. Lancer depuis Aroma

## Test

1. Lancer le serveur d'écho sur PC:
   ```bash
   python3 echo_server.py
   ```

2. Lancer le test sur Wii U

3. Vérifier:
   - Écran TV: statut réseau, tests en cours
   - Logs UDP sur PC (si configuré)
   - Sortes TCP/UDP dans le terminal du serveur
   - HOME pour quitter proprement

## Logs

Les logs UDP sont envoyés vers `192.168.2.100:18880` par défaut.

Pour capturer:
```bash
nc -ul 18880 < /tmp/wiiu.log
# ou
udplogserver.py
```

Les logs sont aussi sauvegardés dans:
```
/tmp/claude-1000/-home-wozt-dev-capture2cloud/6315e2eb-6d9a-4943-b8d3-f37085cf7809/scratchpad/wiiu.log
```

## Configuration

Modifier `SERVER_IP` dans `main.c` si nécessaire:
```c
#define SERVER_IP "192.168.2.100"  // IP de votre PC
```

## Résolution des problèmes

### "Failed to init UDP log"
- Vérifier que le réseau est actif
- Vérifier que 192.168.2.100 est joignable

### Sockets retournent vers 192.168.2.124 (Wii U native)
- Le shim n'est pas installé → vérifier module Aroma
- Vérifier logs: "24 nsysnet hooks installed"

### Test TCP/UDP échoue
- Vérifier que le serveur echo tourne sur PC
- Vérifier firewall (autoriser port 18879)
- Vérifier source IP dans logs (doit être 192.168.2.190)

## Architecture

Le test utilise les sockets nsysnet standards. Si le shim fonctionne:
- Les sockets sont interceptés par `nsysnet_shim`
- Redirigés vers lwIP sur l'adaptateur AX88179
- Source IP = IP de l'adaptateur (192.168.2.190)
- Destination = PC (192.168.2.100:18879)

## Notes

- Le module Aroma doit être chargé AVANT de lancer le test
- Le test valide que les sockets vont bien par l'Ethernet USB
- HOME/MINUS fonctionnent même si le réseau est instable
