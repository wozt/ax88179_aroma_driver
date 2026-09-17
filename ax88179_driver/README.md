# AX88179 Wii U — pilote Ethernet usermode

Implémentation commune : `../driver/ax88179.c`. `ax88179_driver` et
`linktest` compilent cette même source. API synchrone, un adaptateur à la
fois, appels sérialisés dans un seul thread. Contrôleur UHS 0, VID/PID
0b95:1790, MTU Ethernet 1500 (trame VLAN jusqu'à 1518 octets sans FCS).

## Cause des tests de registres incorrects

Lire la MAC avec `bRequest=0x01` lit **déjà un registre du chip** : NODE_ID,
adresse 0x10. Aucun autre opcode de « déverrouillage » n'est requis.
Les anciennes sondes utilisaient une mauvaise table de commandes :
`0x02` accède au PHY, `0x03` n'est pas la lecture générique des registres,
`0x07` n'est pas l'accès PHY AX88179, `0x05` concerne l'eFuse et ne lit pas
un en-tête RX bulk. La valeur 0 retournée par cette dernière sonde ne
validait donc pas le chemin RX. Le code d'erreur -2162724 est conservé ;
sa signification IOSU exacte n'est pas établie ici.

Les paramètres scalaires de UhsSubmitControlRequest restent en ordre
natif PPC ; **seul le contenu des registres multioctets est little-endian**.
Les buffers DMA sont alignés sur 64 octets avec flush/invalidation cache.

| Accès | bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|---|
| Lire MAC/chip | 0xC0 | 0x01 | registre | taille | taille |
| Écrire MAC/chip | 0x40 | 0x01 | registre | taille | taille |
| Lire PHY | 0xC0 | 0x02 | 0x0003 | registre MII | 2 |
| Écrire PHY | 0x40 | 0x02 | 0x0003 | registre MII | 2 |

Exemple BMSR : `(buffer, 0x02, 0xC0, 3, 1, 2, 1000)` après handle et
if_handle. Ne pas confondre bRequest=0x02 avec registre MAC 0x02 : ce
registre donne la vitesse USB, pas la vitesse Ethernet.

## Séquence appliquée

1. Acquérir l'interface et identifier les endpoints par type/direction.
2. Écrire MAC 0x26, 2 octets : `00 00`, puis `20 00` (IPRL).
   C'est la séquence de reset/alimentation PHY du pilote de référence,
   pas une commande USB « soft reset 0x09 ». Attendre 500 ms.
3. Écrire MAC 0x33, 1 octet : `03` (ACS|BCS). Attendre 200 ms.
   Relire 0x33 et vérifier `(valeur & 3) == 3`, puis lire NODE_ID.
4. Arrêter RX (0x0B=0) et le medium (0x22=0). Écrire les 5 octets de
   RX_BULKIN_QCTRL à 0x2E : `07 20 03 16 FF`, profil initial USB HS/Gigabit.
   LOW 0x55=0x34, HIGH 0x54=0x52 ; RX/TX checksum offload 0x34/0x35=0.
5. PHY page 0 : registre 0x1F=0 ; lire BMCR (0), préserver ses valeurs de
   vitesse, enlever reset/loopback/powerdown/isolate, ajouter 0x1200
   (autoneg enable + restart). Aucune écriture EEPROM/eFuse.
6. Administrer séparément les endpoints bulk IN et OUT. Une requête en
   attente par endpoint ; buffers IN 26 Kio et OUT 2048 octets.
   Chaque retour est vérifié. Démarrer RX avec MAC 0x0B=0x03AA.
7. Poller `ax88179_link` : lire BMSR deux fois (bit link latched-low),
   exiger lien + autoneg terminée, puis lire PHY 0x11 : lien bit 0x0400,
   duplex 0x2000, vitesse masque 0xC000 (0x8000=1000, 0x4000=100, 0=10).
8. À chaque changement de lien/vitesse/duplex, appliquer la file RX et
   le medium ; attendre la disponibilité FIFO TX (requête 0x81,
   wValue=0x8C, wIndex=0, longueur 4, bit 30), avec attente bornée.
   Activer medium RECEIVE_EN=0x0100, duplex=0x0002 si full, Gigabit=0x0009
   ou 100 Mbit/s=0x0200. Pause Ethernet désactivée en attendant une vraie
   négociation pause ; aucun jumbo/TSO/offload.

Files RX : Gigabit USB SS `07 4F 00 12 FF`, Gigabit USB HS
`07 20 03 16 FF`, 100 Mbit/s USB HS/SS `07 AE 07 18 FF`, sinon
`07 CC 4C 18 08`. Le plus grand profil demande `(0x18+2)*1024=26624`
octets, d'où le buffer de 26 Kio (l'ancien buffer de 16 Kio était trop petit).

## Particularités UHS à vérifier sur console

Le masque AdministerEndpoint suit wut UHSEndpointGetMask : **OUT aux bits
0..15, IN aux bits 16..31**. Bulk IN 2 = `0x00040000`, OUT 3 = `0x00000008`,
ensemble = `0x00040008`. Les directions SubmitBulkRequest sont un argument
séparé : OUT=1, IN=2, endpoint numéro sans le bit 0x80.

La version « diag 3 » corrige le masque inversé utilisé par « diag 2 ».
Les anciennes conclusions de `../FINDINGS.md` sur ces masques sont
erronées (voir rectification en tête de ce fichier). Le worker local
utilise direction 1 pour les bits bas et 2 pour les bits hauts. Sa fonction
de recherche renvoie un slot calculé sans vérifier sa présence : recevoir
-2162715 avec le masque inversé ne prouve pas une restriction usermode.

Les lectures UHS locales renvoient le nombre d'octets (MAC=6). Le pilote
vérifie les tailles exactes pour les transferts control et TX ; confirmer
également ce contrat pour OUT sur console. Un retour court apparaît dans
le diagnostic avec la requête exacte, sans être accepté comme succès.

Si `-2162715` persiste avec **IN=00040000**, conserver ce diagnostic et
vérifier acquisition/permissions UHS séparément. Ce programme ne modifie
pas IOSU. Le résultat du test console est nécessaire avant de conclure
sur les transferts bulk.

## Construire et tester

```sh
make -C wiiu_ethernet/ax88179_driver -j2
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  wiiu_ethernet/tests/test_ax88179.c -o /tmp/test_ax88179
/tmp/test_ax88179
```

Copier `ax88179_driver.wuhb` dans `sd:/wiiu/apps/`, puis lancer avec
l'adaptateur et un câble relié à un pair actif. L'application affiche :

- MAC, endpoints et registres MAC 0x26/0x33/0x0B/0x22 ;
- PHY BMCR, BMSR, identifiants 2/3 et statut 0x11 ;
- négociation jusqu'à 15 secondes et medium final (full duplex :
  0x0102 à 10, 0x0302 à 100, 0x010B à 1000 Mbit/s) ;
- envoi d'une trame broadcast de test EtherType expérimental 0x88B5 ;
- comptage de trames reçues et erreurs UHS brutes.

Sur un PC directement relié, vérifier TX avec :

```sh
sudo tcpdump -eni <interface> ether proto 0x88b5
```

Une soumission TX positive ne prouve pas l'émission sur le câble : la
capture du pair fait cette vérification. Générer du trafic broadcast
depuis le pair pour RX. Les tests hôte couvrent les setups, les erreurs
et nettoyages, le lien, le padding TX et les limites RX ; ils ne prouvent
ni le comportement DMA réel ni les permissions/transferts IOSU.

## Intégration IP

Le pilote fournit des trames Ethernet (`send`/`receive`) et le lien.
Il n'installe pas une interface réseau dans les sockets système Wii U.
DHCP exige une pile IP usermode, par exemple un port lwIP raccordé à ces
fonctions, avec ses checksums logiciels puisque l'offload est désactivé.
DHCP et l'intégration de cette pile ne sont pas implémentés ici.

## Références

- [Pilote Linux AX88179, registres/reset/link/RX/TX](https://github.com/torvalds/linux/blob/master/drivers/net/usb/ax88179_178a.c).
- [Signatures UHS de wut](https://github.com/devkitPro/wut/blob/master/include/nsysuhs/uhs.h).
- `../reference/ax88179_178a.c` et `../FINDINGS.md` : références locales.
