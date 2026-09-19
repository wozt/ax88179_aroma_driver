# AX88179 Wii U Aroma - Current Findings

[✓] IPv4 / ARP
[✓] DHCP
[✓] ICMP
[✓] TCP client
[✓] UDP
[✓] select
[✓] nonblocking
[✓] errno / SO_ERROR
[✓] socket option translation principale
[✓] SNDBUF / RCVBUF ABI mesurée
[✓] IP_MULTICAST_TTL
[✓] IP_MULTICAST_LOOP
[✓] IP_MULTICAST_IF
[✓] IP_ADD_MEMBERSHIP
[✓] IP_DROP_MEMBERSHIP
[✓] réception multicast réelle
[✓] émission multicast réelle

[✓] limites négatives exactes SNDBUF/RCVBUF
[ ] parité exacte SNDBUF / backpressure (écart résiduel à 4096)
[✓] parité réelle RCVBUF / SO_RXDATA
[✓] TCP listen / accept réel
[✓] sendto_multi_ex
[✓] recvfrom_multi
[✓] recvfrom_ex avec vrai TTL
[✓] sendto_multi
[ ] parité UDP SO_RXDATA (natif = payload + 16 octets/datagramme)
[ ] gethostbyaddr
[ ] DNS async / variantes restantes
[ ] NSSL Nintendo : détourner le transport TLS vers lwIP/AX
[✓] hot-unplug / reconnect
[ ] perte/restauration link
[ ] DHCP renew/recovery
[✓] exhaustion sockets
[✓] exhaustion buffers (capacité native atteinte à débit soutenable)
[ ] RX burst haute cadence (~147 Mbit/s)
[ ] charge/concurrence

## État actuel

Module :

    0.2.22-async-usb-rx-ring-align

Configuration normale validée :

```ini
[dhcp]
mode=keep_first

[debug]
shim_trace=0

[compat]
dns=system
route=ax
```

Adresses utilisées pendant les tests :

    Wii U réseau natif : 192.168.2.124
    AX88179 / lwIP     : 192.168.2.190
    PC de test         : 192.168.2.100

Architecture actuelle :

    titre Wii U
        |
        | nsysnet
        v
    shim Aroma
        |
        +---- DNS système/native
        |
        +---- sockets GAME compatibles
                  |
                  v
                lwIP
                  |
                  v
              AX88179
                  |
                  v
             USB Ethernet

NSSL utilise actuellement un chemin hybride décrit plus bas.

## Jeux réels validés

### Mario Maker + Pretendo

Validé jusqu'au gameplay réel :

- Course World fonctionne ;
- téléchargement d'un niveau ;
- lancement du niveau ;
- jeu du niveau téléchargé.

#### NSSL promotion

NSSL ne peut pas utiliser directement un fd appartenant à lwIP.

Lors d'un `NSSLCreateConnection()` sur une socket AX :

1. le peer de la socket lwIP est récupéré ;
2. le placeholder nsysnet natif réservé est connecté au même peer ;
3. la socket cesse d'être possédée par AX/lwIP ;
4. la socket lwIP est fermée ;
5. NSSL reçoit la socket native connectée.

Le trafic TLS/NSSL est donc actuellement promu vers le réseau natif.

Cette promotion est une solution provisoire.

Architecture finale visée :

    jeu
     |
     +------ trafic normal ------> nsysnet shim -> lwIP
     |
     +------ NSSL Nintendo ------> TLS Nintendo
                                      |
                                pont transport
                                      |
                                      v
                                    lwIP
                                      |
                                      v
                                   AX88179

L'objectif est donc de conserver l'implémentation NSSL/TLS Nintendo et
de détourner uniquement son transport vers lwIP, si le reverse
engineering permet de le faire proprement.

#### SO_TCPSACK

Sur Wii U :

    SOL_SOCKET / 0x0200 = SO_TCPSACK

Dans lwIP, la même valeur numérique désigne une autre option.

Le shim traite donc `SO_TCPSACK` explicitement comme un no-op compatible.

Règle générale :

    ne jamais supposer que les numéros d'options nsysnet == lwIP.

### Minecraft Wii U Edition + Pretendo

Validé jusqu'au gameplay :

- sockets GAME via AX/lwIP ;
- Pretendo actif ;
- connexion réseau ;
- création d'une map ;
- gameplay fonctionnel.

Le tracing socket très verbeux peut casser NEX même en route native.

Configuration normale :

    shim_trace=0

Les hooks sensibles au timing doivent rester silencieux.

## Cycle de vie du module

Le worker réseau est recréé lors des transitions de titres.

Gardes actuellement utilisées :

    première ouverture Aroma : ~25 s
    transitions suivantes    : ~2 s

Le PHY est conservé lors d'une réouverture lorsque son état est valide.

Mesures typiques :

    cold open : ~750 ms
    link cold : ~3.1 s

    warm open : ~250 ms
    link warm : ~13-15 ms

Avec `dhcp=keep_first`, la première lease DHCP de la session Aroma est conservée en RAM et réutilisée après les transitions.

Une nouvelle négociation DHCP est toujours faite après reboot complet.

## nsysnet / errno

Mapping brut nsysnet caractérisé :

    nerr 1  = ENOBUFS
    nerr 6  = EWOULDBLOCK
    nerr 11 = EINVAL
    nerr 12 = EMSGSIZE
    nerr 51 = EMFILE

Attention :

    errno WUT != socketlasterr() brut nsysnet

## Capacité des sockets

Référence native :

    28 sockets titre
    fd visibles 4..31

TCP seul : 28
UDP seul : 28
mixte : 28

29e socket :

    errno = EMFILE
    nerr  = 51

Après fermeture :

    réouverture immédiate fd=4

Stable sur plusieurs rounds sans fuite.

AX reproduit cette limite grâce aux placeholders nsysnet publics et au mapping explicite public-fd -> lwIP-fd.

Configuration lwIP interne :

    LWIP_SOCKET_OFFSET = 0
    MEMP_NUM_TCP_PCB   = 32
    MEMP_NUM_UDP_PCB   = 32
    MEMP_NUM_NETCONN   = 32

## Coexistence native + AX

Validé :

- sockets AX et natives simultanées ;
- socket native conservée sur 192.168.2.124 ;
- sockets AX sur 192.168.2.190 ;
- poll/select mixte ;
- TCP et UDP concurrents sans collision de fd.

## TCP listen / accept

Validé nativement puis via AX :

- bind ;
- listen ;
- backlog ;
- accept ;
- getsockname / getpeername ;
- SO_TYPE ;
- transfert court ;
- transfert 128 KiB ;
- hash identique ;
- EOF ;
- `shutdown(SHUT_WR)` / half-close.

## TCP SO_SNDBUF / backpressure

Le shim implémente maintenant une vraie limite de buffer d'émission par PCB.

Référence native importante :

    default visible = 8192

Les valeurs négatives de SNDBUF/RCVBUF sont conservées exactement côté ABI visible.

Cas validés fonctionnellement :

    SNDBUF=0
    SNDBUF=1
    SNDBUF=8192
    SNDBUF=16384
    SNDBUF=65535

La récupération après backpressure fonctionne.

Écart encore ouvert :

    SNDBUF=4096

Le comportement est fonctionnel mais le nombre exact d'octets acceptés avant EWOULDBLOCK diffère encore du natif.

## TCP SO_RCVBUF / SO_RXDATA

Parité fonctionnelle validée.

    RCVBUF       natif       AX
    default      8192        8192
    1            1           1
    4096         4096        4096
    8192         8192        8192
    16384        16384       16384
    65535        65535       65535

Le sender subit réellement le backpressure TCP à la limite demandée.

Le drain réouvre la fenêtre.

Le cas RCVBUF=1 reproduit le comportement volontairement pathologique du natif, avec une progression extrêmement lente.

## APIs UDP multi-datagrammes

Validées :

    sendto_multi
    sendto_multi_ex
    recvfrom_multi

Contraintes ABI natives trouvées :

- structures/buffers sensibles à un alignement 0x40 ;
- tailles/capacités arrondies/paddées à 0x40 ;
- timeout brut nsysnet :

      struct {
          int32_t sec;
          int32_t usec;
      };

  soit 8 octets, et non `struct timeval` libc.

## recvfrom_ex / TTL

`MSG_IP_RECVTTL = 0x40`

Comportement natif reproduit :

- flags=0 : metadata extra mise à zéro ;
- flag `0x40` : `extra[0]` contient le vrai TTL IPv4 ;
- reste de la zone extra mis à zéro ;
- bytes après `extra_len` non modifiés ;
- `extra_len=0` est invalide même sans flag ;
- dans ce cas le datagramme n'est pas consommé.

lwIP conserve maintenant le TTL reçu dans le netbuf jusqu'au shim.

## getsockname UDP connecté

Pour une UDP connectée créée sur wildcard, le natif expose l'adresse locale réellement sélectionnée.

Le shim substitue maintenant l'IP AX :

    192.168.2.190

au lieu de laisser une adresse wildcard.

## Multicast

Validé :

    IP_MULTICAST_TTL
    IP_MULTICAST_LOOP
    IP_MULTICAST_IF
    IP_ADD_MEMBERSHIP
    IP_DROP_MEMBERSHIP

Validation réelle :

- réception multicast ;
- émission multicast.

## UDP : capacité des queues

Probe :

    8 sockets
    SO_RCVBUF=65535
    64 datagrammes/socket
    1400 octets/datagramme
    512 datagrammes envoyés

### Référence native

Chaque socket conserve exactement :

    46 datagrammes
    64400 octets payload
    SO_RXDATA=65136

Total :

    368 datagrammes
    515200 octets payload

Récupération après drain :

    sockets=8/8
    packets=24/24

stable sur trois rounds.

Relation observée :

    46 * 1416 = 65136

Le `SO_RXDATA` UDP natif compte donc apparemment :

    payload + 16 octets par datagramme

AX compte actuellement seulement le payload :

    46 * 1400 = 64400

Cette différence de comptabilité reste ouverte.

### Alignement recvfrom natif

Avec des buffers stack non alignés, le probe recevait :

    socketlasterr=12
    EMSGSIZE

Avec :

    data      aligned(0x40)
    sockaddr  aligned(0x40)

`recvfrom()` fonctionne normalement.

## Pools lwIP UDP actuels

    MEMP_NUM_NETBUF            = 512
    PBUF_POOL_SIZE             = 512
    DEFAULT_UDP_RECVMBOX_SIZE  = 64

Ces valeurs permettent d'atteindre la même capacité socket que le natif :

    368 datagrammes
    515200 octets

lorsque le chemin RX peut suivre le débit d'entrée.

## Pipeline RX haute cadence

Le problème actuel n'est plus la capacité mémoire des sockets.

Matrice UDP :

    8 sockets
    datagrammes de 1400 octets

### Avant ring USB async

Avec entrée lwIP async mais réception USB non pipelinée :

    ~145 Mbit/s : 91 / 368
    ~42 Mbit/s  : 368 / 368
    ~17 Mbit/s  : 368 / 368

### Test priorité tcpip rejeté

Test :

    tcpip Coreinit priorité 17
    AX worker priorité 16

Résultat :

    ~146 Mbit/s : 40 / 368
    ~42 Mbit/s  : 216 / 368
    ~17 Mbit/s  : 368 / 368

Régression nette.

La priorité tcpip a donc été restaurée à :

    TCPIP_THREAD_PRIO=1
    -> Coreinit priorité 5

### Ring USB RX asynchrone

Implémentation actuelle :

    RX_ASYNC_SLOTS = 3

Trois `UhsSubmitBulkRequestAsync()` sont gardés en vol.

Chaque slot possède son propre buffer DMA.

Lorsqu'un bulk termine :

1. il est copié dans le snapshot parser `g_rx` ;
2. le slot DMA est immédiatement réarmé ;
3. les trames du snapshot sont ensuite envoyées vers lwIP.

Les buffers DMA sont alignés sur :

    0x40

Un premier essai avec `aligned(0x100)` faisait refuser le module par WUMSLoader :

    MODULE_LINK_ERROR_ADDRESS_UNALIGNED

`0x40` charge correctement.

### Dernière mesure — état actuel

Module :

    0.2.22-async-usb-rx-ring-align

Résultat :

    gap 500 us
    actual ~146.7 Mbit/s
    296 / 368 datagrammes
    414400 octets

    gap 2000 us
    actual ~42.4 Mbit/s
    368 / 368 datagrammes
    515200 octets

    gap 5000 us
    actual ~17.4 Mbit/s
    368 / 368 datagrammes
    515200 octets

Tous les rounds :

    RECOVERY sockets=8/8
    packets=24/24

Le ring USB async a donc fait progresser le fast-burst :

    91 -> 296 datagrammes

soit environ 80 % de la capacité native testée à ~147 Mbit/s.

Le ring UHS async est donc validé comme amélioration majeure, mais la parité haute cadence n'est pas encore atteinte.

## Configuration RX/lwIP actuelle

    PBUF_POOL_SIZE             = 512
    MEMP_NUM_NETBUF            = 512
    MEMP_NUM_TCPIP_MSG_INPKT   = 512
    TCPIP_MBOX_SIZE            = 512

    DEFAULT_UDP_RECVMBOX_SIZE  = 64
    DEFAULT_TCP_RECVMBOX_SIZE  = 64

    TCP_WND                    = 65535

    LWIP_TCPIP_CORE_LOCKING       = 1
    LWIP_TCPIP_CORE_LOCKING_INPUT = 0

    TCPIP_THREAD_PRIO          = 1
    -> Coreinit priorité 5

AX worker :

    Coreinit priorité 16

watchdog :

    Coreinit priorité 24

## Règles de debug importantes

### Ne pas logger dans les hooks sensibles

Le logging direct dans certains hooks socket/NSSL peut :

- modifier le timing ;
- perturber `socketlasterr()` ;
- casser des séquences NEX.

Configuration normale :

    shim_trace=0

Utiliser plutôt :

- compteurs RAM ;
- traces différées ;
- logs depuis le worker.

### HOME des WUHB

Le comportement correct est :

    HOME -> overlay système Aroma -> Quit

Ne pas intercepter HOME pour tenter de relancer directement le menu.

## État des hooks

Le shim couvre notamment :

    socket
    socketclose
    socketclose_all
    bind
    connect
    listen
    accept
    shutdown
    send
    sendto
    sendto_multi
    sendto_multi_ex
    recv
    recvfrom
    recvfrom_ex
    recvfrom_multi
    select
    getsockname
    getpeername
    setsockopt
    getsockopt
    socketlasterr
    NSSLCreateConnection

Avec `dns=system`, les hooks DNS restent volontairement désactivés.

## Travail restant prioritaire

### Compatibilité ABI

- écart exact SNDBUF=4096 ;
- UDP SO_RXDATA : ajouter les 16 octets/datagramme visibles ;
- gethostbyaddr ;
- DNS async / variantes restantes ;
- état/options NSSL lors de la promotion.

### Robustesse réseau

- hot-unplug/reconnect ;
- perte/restauration du link ;
- DHCP renew/recovery ;
- charge/concurrence longue durée.

### Performance RX

Objectif du probe actuel :

    368 / 368 datagrammes

à environ :

    147 Mbit/s payload

État actuel :

    296 / 368

Prochaine session :

- caractériser où disparaissent les 72 datagrammes restants ;
- mesurer les complétions des trois slots UHS ;
- vérifier si 3 slots suffisent ou si davantage de requêtes en vol aident ;
- vérifier l'agrégation/FIFO AX88179 avant de modifier encore lwIP.

## Références pratiques

Build module :

```bash
make -C aroma_module clean
make -C aroma_module -j"$(nproc)"
```

Upload module :

```bash
curl --ftp-pasv --user anonymous: \
  -T aroma_module/AX88179Module.wms \
  ftp://192.168.2.124/fs/vol/external01/wiiu/environments/aroma/modules/AX88179Module.wms
```

Routes :

```bash
./tools/toggle_route.sh native
./tools/toggle_route.sh ax
./tools/toggle_route.sh status
```

Shutdown :

```bash
./tools/shutdown_wiiu.sh
```

Probe buffer/RX :

```bash
python3 tools/buffer_exhaustion_probe/peer.py
```

Current expected AX path :

    192.168.2.190


## Hot-unplug USB - comportement avant recovery

Test effectué avec l'AX88179 actif sur :

    192.168.2.190

Avant débranchement, le ping fonctionnait normalement.

Après arrachement physique du dongle USB, les réponses ont cessé et le
PC a fini par retourner :

    Destination Host Unreachable

Le rebranchement du dongle n'a pas restauré automatiquement le chemin AX
dans l'implémentation courante.

Le code expliquait ce comportement :

    ax88179_link() -> erreur UHS
            |
            v
       link_errors++
            |
       après 3 erreurs
            |
            v
    ax_net_poll() -> -2

mais la boucle principale ne fermait ni ne recréait le handle UHS.

Le correctif suivant ajoute :

- netif down / arrêt AX courant ;
- annulation des bulk RX async ;
- fermeture UHS ;
- retry périodique de l'AX88179 ;
- réinitialisation PHY froide après replug physique ;
- recréation du netif ;
- restauration de la lease DHCP de session en mode keep_first ;
- reprise du polling sans reboot de la Wii U.

Une erreur séparée a aussi été trouvée dans le chemin link-down :

    ax_net_address() peut retourner NULL

alors que la boucle principale utilisait ensuite :

    if (!ip[0])

Le test câble Ethernet suivant nécessite donc également de rendre ce test
NULL-safe.


## Hot-unplug USB / reconnect validé

Le recovery automatique après arrachement physique du dongle AX88179 est
maintenant fonctionnel.

Scénario validé :

    AX actif sur 192.168.2.190
        |
        v
    débranchement physique USB
        |
        v
    192.168.2.190 devient inaccessible
        |
        v
    erreurs UHS / PHY détectées
        |
        v
    netif arrêté
        |
        v
    ancien handle UHS fermé
        |
        v
    dongle rebranché sur le même port
        |
        v
    nouvel open AX88179
        |
        v
    initialisation PHY froide
        |
        v
    ring RX async recréé
        |
        v
    lease de session restaurée
        |
        v
    192.168.2.190 répond à nouveau

Logs observés :

    AX: hotplug recovered 192.168.2.190
    AX: ready 192.168.2.190
    AX: udp-log 192.168.2.190

Le ping est revenu automatiquement sans reboot ni changement de titre.

La première réponse après recovery a subi environ 1 seconde de latence,
puis les réponses suivantes sont revenues aux valeurs normales de quelques
millisecondes.

Le recovery a été observé plusieurs fois dans la même session.

Conclusion :

    hot-unplug / reconnect : VALIDÉ
