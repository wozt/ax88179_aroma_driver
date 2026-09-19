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
[✓] perte/restauration link
[✓] DHCP recovery après recréation interface
[ ] DHCP renew/rebind à expiration de lease
[✓] exhaustion sockets
[✓] exhaustion buffers (capacité native atteinte à débit soutenable)
[ ] RX burst haute cadence (~147 Mbit/s)
[✓] charge/concurrence

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


## Perte / restauration du link Ethernet validée

Le câble RJ45 a été débranché puis rebranché en laissant le dongle
AX88179 connecté en USB.

Comportement observé lors de la perte du lien :

    PHY: link=DOWN
    AX: link/lease unavailable
    AX: link=0 ...

L'interface USB est restée ouverte et aucun recovery hotplug n'a été
déclenché.

Après rebranchement du câble :

    PHY: link=UP
    AX: ready 192.168.2.190
    AX: udp-log 192.168.2.190

Le ping vers 192.168.2.190 est revenu automatiquement sans :

- reboot ;
- changement de titre ;
- réouverture UHS ;
- intervention manuelle.

La première réponse après restauration du lien a subi environ une seconde
de latence, puis les réponses sont revenues à quelques millisecondes.

Conclusion :

    perte/restauration link : VALIDÉ


## DHCP recovery validé

Le mode :

    dhcp=always

a été testé avec une véritable recréation de l'interface après
hot-unplug USB.

Séquence validée :

    AX88179 actif
        |
        v
    lease DHCP obtenue
        |
        v
    unplug USB
        |
        v
    interface UHS perdue
        |
        v
    ancien netif supprimé
        |
        v
    replug USB
        |
        v
    nouvel AX88179 open
        |
        v
    nouveau netif lwIP
        |
        v
    nouveau cycle DHCP
        |
        v
    192.168.2.190 opérationnel

Log observé au démarrage :

    AX: config dhcp=always trace=0 dns=system route=ax
    AX: lease DHCP 192.168.2.190

Après hotplug :

    AX: hotplug recovered 192.168.2.190
    AX: ready 192.168.2.190

Le chemin réseau et le ping reviennent automatiquement sans reboot.

Le test RJ45 seul a également été répété en mode dhcp=always.

Pendant la coupure câble, le client reste normalement dans son état
BOUND :

    dhcp=10

puis le trafic reprend immédiatement lorsque le PHY retrouve le link.
Il n'est donc pas nécessaire de refaire artificiellement un DHCP à
chaque perte temporaire du câble Ethernet.

Validé :

    [✓] DHCP recovery après recréation interface

Reste à caractériser séparément :

    [ ] renew/rebind lors d'une véritable expiration ou modification
        de lease DHCP


## Charge / concurrence validée

Un stress test prolongé a été effectué simultanément avec :

    4 sockets TCP AX
    4 sockets UDP AX
    1 socket UDP native de coexistence

Chaque worker AX a exécuté :

    8192 rounds

avec alternance de payloads :

    32
    504
    1024
    1400 octets

Résultat total :

    65536 échanges AX
    48496640 octets de payload vérifié

Tous les workers ont terminé sans erreur :

    TCP worker 0 : 8192/8192 errno=0
    TCP worker 1 : 8192/8192 errno=0
    TCP worker 2 : 8192/8192 errno=0
    TCP worker 3 : 8192/8192 errno=0

    UDP worker 4 : 8192/8192 errno=0
    UDP worker 5 : 8192/8192 errno=0
    UDP worker 6 : 8192/8192 errno=0
    UDP worker 7 : 8192/8192 errno=0

Résultat global :

    AXSTRESS result=PASS
    AXCONCURRENT result=PASS

Aucune corruption de payload, erreur socket, collision de fd ou timeout
n'a été observé.

La socket native créée avant activation du shim est restée correctement
routée sur :

    192.168.2.124

pendant que les sockets AX utilisaient :

    192.168.2.190

La coexistence native + AX reste donc correcte sous charge prolongée.

Conclusion :

    charge / concurrence : VALIDÉ


## NSSL transport : loopback natif validé

La primitive nécessaire au futur pont NSSL a été caractérisée directement
sur la Wii U avec de vrais descripteurs nsysnet créés avant l'activation
du shim AX.

Test :

    listener natif
        |
        +-- bind 127.0.0.1:0
        |
        +-- listen
        |
        +-- client natif -> connect 127.0.0.1
        |
        +-- accept
        |
        +-- transfert bidirectionnel 65536 octets

Résultat observé :

    LOOPBACK bind(127.0.0.1:0) rc=0 errno=0
    LOOPBACK listen rc=0 errno=0
    LOOPBACK listener=127.0.0.1:2065
    LOOPBACK client SO_ERROR rc=0 errno=0 value=0

    client:
        local=127.0.0.1:2066
        peer=127.0.0.1:2065

    accepted:
        local=127.0.0.1:2065
        peer=127.0.0.1:2066

    LOOPBACK CLIENT->SERVER 65536 bytes PASS
    LOOPBACK SERVER->CLIENT 65536 bytes PASS

    LOOPBACK RESULT: PASS

Le shim AX s'est activé pendant le test sans casser les descripteurs natifs
créés auparavant.

Conclusion :

Le nsysnet natif de la Wii U peut servir de transport local entre
IOS-NSEC/NSSL et un relay PPC.

L'architecture suivante est donc techniquement testable :

    Nintendo NSSL / IOS-NSEC
              |
         socket natif
              |
          127.0.0.1
              |
         relay PPC
              |
          lwIP socket
              |
           AX88179


## NSSL Nintendo transporté via lwIP / AX88179

Le premier pont réel entre Nintendo NSSL et le réseau AX88179 a été
validé.

Architecture testée :

    jeu
      |
      v
    Nintendo NSSL / IOS-NSEC
      |
      | TLS Nintendo
      v
    socket nsysnet natif
      |
      v
    127.0.0.1
      |
      v
    relay PPC
      |
      v
    socket lwIP déjà connectée
      |
      v
    AX88179
      |
      v
    Internet / Pretendo

Le hostname original est conservé dans NSSLCreateConnection().
Seul le transport sous TLS est remplacé.

Connexions observées :

    discovery.olv.pretendo.cc
    s3.pretendo.cc

Exemple :

    AX: NSSL bridge ready fd=6 lwfd=2 loopback_port=4599
    AX: NSSL bridge setup fd=6 result=1 host=discovery.olv.pretendo.cc
    AX: NSSLCreateConnection fd=6 result=0 mode=bridge

Le relay observe ensuite du trafic dans les deux directions :

    AX: NSSL relay first NSSL->AX bytes=208
    AX: NSSL relay first AX->NSSL bytes=4096

Une connexion vers s3.pretendo.cc a transféré :

    NSSL -> AX : 757 octets
    AX -> NSSL : 458715 octets

Sur l'ensemble du log fourni :

    8 connexions NSSL bridgées
    environ 6323 octets NSSL -> AX
    environ 765797 octets AX -> NSSL

Le volume reçu dépasse très largement un simple handshake TLS :
du trafic HTTPS applicatif est donc effectivement transporté par
lwIP/AX88179.

Deux connexions se sont terminées avec :

    err=3013

Le relay encode cette valeur comme :

    3000 + socketlasterr()

donc :

    socketlasterr = 13 = EPIPE

Cela correspond à une fermeture du côté NSSL alors que le relay avait
encore des données à pousser. Les autres connexions se terminent avec
err=0.

Conclusion :

    [✓] Nintendo NSSL fonctionne au-dessus d'un tunnel localhost
    [✓] les octets TLS sortent via lwIP
    [✓] les réponses Internet reviennent via AX88179
    [✓] aucun remplacement de l'implémentation TLS Nintendo nécessaire

Le transport NSSL natif direct vers Internet n'est plus nécessaire en
mode nssl=bridge.


## NSSL relay : fermeture propre validée

Après validation initiale du transport Nintendo NSSL via localhost,
le relay a été corrigé pour traiter comme fermeture normale les erreurs
nsysnet correspondant à une fermeture du peer local NSSL.

Le test Mario Maker / Pretendo a ensuite été répété.

Connexions observées :

    discovery.olv.pretendo.cc
    s3.pretendo.cc

Huit connexions NSSL ont été bridgées successivement.

Pour chacune :

    NSSL bridge setup result=1
    NSSLCreateConnection result=0

Le trafic TLS a été observé dans les deux directions :

    NSSL -> localhost -> relay -> lwIP -> AX88179
    AX88179 -> lwIP -> relay -> localhost -> NSSL

Toutes les connexions se terminent maintenant avec :

    err=0

Exemple de transfert important :

    AX: NSSL relay end slot=0
        nssl_to_ax=757
        ax_to_nssl=458684
        err=0

Résultat :

    [✓] création du tunnel localhost
    [✓] handshake NSSL à travers AX
    [✓] trafic HTTPS applicatif à travers AX
    [✓] fermeture normale côté NSSL
    [✓] cleanup relay sans erreur

Conclusion :

Le transport Nintendo NSSL via :

    IOS-NSEC
        |
    127.0.0.1
        |
    relay PPC
        |
    lwIP
        |
    AX88179

est désormais fonctionnel et se ferme proprement.


## Super Smash Bros. for Wii U : match en ligne réel validé

Un test réel a été effectué avec Super Smash Bros. for Wii U sur Pretendo.

Le jeu a réussi à :

    - accéder aux services en ligne
    - établir les connexions NSSL nécessaires
    - effectuer le matchmaking
    - trouver un véritable joueur distant
    - démarrer une partie
    - jouer la partie en ligne

Les connexions NSSL observées concernaient notamment :

    discovery.olv.pretendo.cc
    api.olv.pretendo.cc

Toutes ont utilisé le bridge :

    Nintendo NSSL
        |
    socket native localhost
        |
    relay PPC
        |
    lwIP
        |
    AX88179

Résultats NSSL :

    NSSL bridge setup result=1
    NSSLCreateConnection result=0
    trafic bidirectionnel observé
    relay end err=0

Le trafic de gameplay lui-même ne correspond pas aux connexions OLV
affichées dans ces logs et passe vraisemblablement par les APIs réseau
de jeu / NEX / P2P via le shim nsysnet.

Le succès d'une partie contre un véritable joueur constitue donc une
validation en conditions réelles bien plus large que le seul bridge TLS.

Conclusion :

    [✓] Smash 4 : accès Pretendo
    [✓] Smash 4 : NSSL via AX88179
    [✓] Smash 4 : matchmaking réel
    [✓] Smash 4 : connexion P2P / gameplay réel
    [✓] Smash 4 : partie en ligne complète


## DNS async : premier audit ABI natif

Un probe passif a résolu et lu le code des exports nsysnet sans appeler
les fonctions DNS non documentées.

Exports observés :

    getaddrinfo          = 0x010c4f38
    getaddrinfo_async    = 0x010c4f60
    getaddrinfo_rs       = 0x010c4f88
    getaddrinfo_async_rs = 0x010c4fb0
    gethostbyaddr        = 0x010c4704
    dns_abort_by_hname   = 0x010c4fd8
    clear_resolver_cache = 0x010c3f8c
    set_resolver_allocator = 0x010c3fb8

Les quatre variantes getaddrinfo appellent le même helper interne :

    0x010c4900

Le wrapper sélectionne le mode via r7 :

    getaddrinfo          : r7 = 0
    getaddrinfo_async    : r7 = 1
    getaddrinfo_rs       : r7 = 0
    getaddrinfo_async_rs : r7 = 1

Le wrapper passe également en r8 une zone de travail locale située à
stack+8.

Tailles de stack observées :

    getaddrinfo          : 0x36c0
    getaddrinfo_async    : 0x36c0
    getaddrinfo_rs       : 0x588
    getaddrinfo_async_rs : 0x2d0

Les registres r3-r6 sont transmis au helper sans modification.

r9/r10 traversent également le wrapper sans être explicitement écrasés ;
il faut donc inspecter le helper avant de conclure définitivement sur le
nombre d'arguments publics des variantes async.

gethostbyaddr montre explicitement :

    r5 comparé à AF_INET (2)
    r4 comparé à 4
    adresse IPv4 lue depuis r3

ce qui concorde avec :

    gethostbyaddr(addr, len, type)

set_resolver_allocator vérifie r3 et r4 et stocke deux pointeurs,
ce qui confirme son ABI à deux arguments.

Aucun appel expérimental n'a été effectué pendant ce test.


## DNS async : helper commun et ABI 4 arguments

Le second audit passif a dumpé 128 instructions du helper commun
getaddrinfo situé à :

    0x010c4900

Les quatre wrappers publics passent par ce helper.

Le prologue montre explicitement :

    r3 -> r27 = node
    r4 -> r28 = service
    r6 -> r29 = res
    r7 -> r30 = mode sync/async
    r8 -> r31 = workspace interne

Le pointeur hints reste en r5 et ses champs sont lus directement :

    +0x00 flags
    +0x04 family
    +0x08 socktype
    +0x0c protocol

Les wrappers injectent eux-mêmes :

    getaddrinfo          : r7 = 0
    getaddrinfo_async    : r7 = 1
    getaddrinfo_rs       : r7 = 0
    getaddrinfo_async_rs : r7 = 1

et :

    r8 = stack + 8

Les arguments publics r3-r6 sont laissés inchangés.

Le helper ne sauvegarde pas r9/r10 comme arguments publics et commence
rapidement à les réutiliser comme registres temporaires.

Conclusion ABI :

    int getaddrinfo(
        const char *node,
        const char *service,
        const struct addrinfo *hints,
        struct addrinfo **res);

    int getaddrinfo_async(
        const char *node,
        const char *service,
        const struct addrinfo *hints,
        struct addrinfo **res);

    int getaddrinfo_rs(
        const char *node,
        const char *service,
        const struct addrinfo *hints,
        struct addrinfo **res);

    int getaddrinfo_async_rs(
        const char *node,
        const char *service,
        const struct addrinfo *hints,
        struct addrinfo **res);

La différence async est interne au resolver et est commandée par r7.

Il reste à caractériser le comportement observable : code retour,
blocage éventuel, résultat immédiat/différé et comportement des variantes
_rs.


## DNS async : comportement natif EAI_INPROGRESS

Les quatre variantes getaddrinfo ont été appelées avec leur ABI native
confirmée à quatre arguments.

Résolution numérique :

    getaddrinfo("127.0.0.1")          -> rc=0, 0 ms
    getaddrinfo_async("127.0.0.1")    -> rc=0, 0 ms
    getaddrinfo_rs("127.0.0.1")       -> rc=0, 0 ms
    getaddrinfo_async_rs("127.0.0.1") -> rc=0, 0 ms

Toutes retournent immédiatement un addrinfo IPv4 valide :

    127.0.0.1:80
    family=2
    socktype=1
    protocol=6

Résolution DNS réelle :

    getaddrinfo("example.com")
        rc=0
        durée=41 ms
        résultat immédiat valide

    getaddrinfo_rs("example.org")
        rc=0
        durée=23 ms
        résultat immédiat valide

En revanche :

    getaddrinfo_async("example.net")
        rc=15 (EAI_INPROGRESS)
        durée=0 ms
        res=NULL

    getaddrinfo_async_rs("iana.org")
        rc=15 (EAI_INPROGRESS)
        durée=0 ms
        res=NULL

Le pointeur res a été conservé dans une zone globale pendant 2 secondes.

Après 2 secondes :

    res reste NULL

Conclusion :

La variante async démarre une résolution en arrière-plan et retourne
EAI_INPROGRESS, mais elle ne complète pas ultérieurement en écrivant
directement dans le struct addrinfo **res fourni au premier appel.

Il reste à déterminer comment le résultat est récupéré. L'hypothèse
suivante à tester est que l'appelant doit rappeler getaddrinfo_async
avec le même hostname jusqu'à ce que la fonction retourne autre chose
que EAI_INPROGRESS.


## DNS async AX : parité native validée

Les variantes DNS supplémentaires ont été implémentées dans le shim AX :

    getaddrinfo_rs
    getaddrinfo_async
    getaddrinfo_async_rs

Le comportement natif avait été mesuré auparavant :

Résolution numérique :

    async("127.0.0.1")
        initial=0
        final=0
        résultat immédiat

Résolution DNS réelle :

    getaddrinfo_async()
        premier appel -> EAI_INPROGRESS (15)
        appels suivants -> EAI_INPROGRESS
        résolution terminée -> 0 + addrinfo

    getaddrinfo_async_rs()
        même comportement

Le shim AX a ensuite été testé directement.

Résultats AX :

    async-num 127.0.0.1
        initial=0
        final=0
        attempts=1
        127.0.0.1:80

    async www.gnu.org
        initial=15
        final=0
        attempts=3
        elapsed=20 ms
        209.51.188.116:80

    async-rs www.archlinux.org
        initial=15
        final=0
        attempts=5
        elapsed=40 ms
        209.126.35.79:80

    sync-rs www.freebsd.org
        initial=0
        final=0
        attempts=1
        elapsed=105 ms
        5.196.63.70:80

Conclusion :

    [✓] getaddrinfo_rs
    [✓] getaddrinfo_async
    [✓] getaddrinfo_async_rs
    [✓] EAI_INPROGRESS natif reproduit
    [✓] polling async natif reproduit
    [✓] résolution DNS async transportée par lwIP / AX88179

Le resolver AX utilise le DNS asynchrone raw de lwIP plutôt qu'un thread
bloquant artificiel. Les réponses terminées sont récupérées depuis le
cache DNS lwIP au polling suivant.


## clear_resolver_cache : une entrée résolue reste disponible

Le comportement natif de clear_resolver_cache() a été mesuré.

Séquence :

    getaddrinfo("www.openbsd.org")
        rc=0
        159 ms

    getaddrinfo_async("www.openbsd.org")
        rc=0
        0 ms

    clear_resolver_cache()

    getaddrinfo_async("www.openbsd.org")
        rc=0
        0 ms
        attempts=1

Conclusion observée :

clear_resolver_cache() ne supprime pas une résolution terminée de la
couche de cache visible par getaddrinfo_async() dans ce scénario.

Il serait donc incorrect d'implémenter immédiatement cet export côté AX
comme un vidage complet de la table DNS lwIP : cela aurait un comportement
plus agressif que le nsysnet natif mesuré.

Le code PowerPC natif de clear_resolver_cache() est un wrapper très mince
qui émet l'ioctl /dev/socket 0x32 vers IOSU.

Le comportement sur une résolution encore EAI_INPROGRESS reste à mesurer
avant de décider de l'implémentation AX.


## clear_resolver_cache : requêtes pending non annulées

Le comportement de clear_resolver_cache() pendant une résolution DNS
asynchrone native réellement en cours a été mesuré.

Séquence :

    getaddrinfo_async("www.netbsd.org")
        rc=15 (EAI_INPROGRESS)
        res=NULL

    clear_resolver_cache()

    attente de 750 ms sans aucun nouvel appel getaddrinfo

    getaddrinfo_async("www.netbsd.org")
        rc=0
        res valide
        151.101.1.6

Conclusion observée :

    [✓] clear_resolver_cache ne supprime pas les résultats positifs déjà
        visibles par getaddrinfo_async

    [✓] clear_resolver_cache n'annule pas une résolution DNS native
        actuellement EAI_INPROGRESS

Il serait donc incorrect d'émuler cet export comme un vidage complet du
cache lwIP ou comme une annulation générale des requêtes en vol.

Le dernier comportement plausible à mesurer est le cache négatif :
clear_resolver_cache pourrait invalider des échecs DNS/NXDOMAIN.


## clear_resolver_cache : aucun effet DNS visible dans les cas mesurés

Trois comportements natifs ont été caractérisés.

### Résultat positif déjà résolu

Après :

    getaddrinfo_async(host) -> rc=0

un appel à :

    clear_resolver_cache()

laisse le même hostname immédiatement disponible :

    getaddrinfo_async(host) -> rc=0

### Résolution encore en cours

Une résolution a été démarrée :

    getaddrinfo_async("www.netbsd.org")
        -> EAI_INPROGRESS

clear_resolver_cache() a été appelé immédiatement.

Après 750 ms sans aucun nouveau polling :

    getaddrinfo_async("www.netbsd.org")
        -> rc=0

La requête en vol n'a donc pas été annulée.

### Échec DNS / NXDOMAIN

Pour :

    ax88179-negative-cache-probe.invalid

comportement observé :

    premier cycle : 15 -> 8
    deuxième cycle : 15 -> 8

Après clear_resolver_cache :

    nouveau cycle : 15 -> 8

L'échec n'est donc pas servi comme une erreur négative immédiatement
mise en cache dans cette API.

Conclusion pratique pour le shim AX :

    [✓] ne PAS vider la table DNS lwIP
    [✓] ne PAS annuler les requêtes lwIP pending
    [✓] conserver clear_resolver_cache en passthrough natif

Un flush lwIP complet serait plus agressif que le comportement natif
mesuré.


## dns_abort_by_hname : premier test natif

Une résolution positive réellement non cachée a été démarrée :

    getaddrinfo_async("www.rust-lang.org")
        -> EAI_INPROGRESS (15)

Puis immédiatement :

    dns_abort_by_hname("www.rust-lang.org")
        -> rc=0

Après 750 ms sans polling :

    getaddrinfo_async("www.rust-lang.org")
        -> rc=0
        -> résultat valide

Le contrôle sans abort donne également :

    getaddrinfo_async("www.llvm.org")
        -> EAI_INPROGRESS
        -> 750 ms
        -> rc=0

Conclusion :

Le résultat DNS continue à devenir disponible après
dns_abort_by_hname() dans ce scénario.

Cela ne suffit toutefois pas à conclure que l'export est un no-op :
les résolutions DNS positives observées sur ce réseau terminent souvent
en environ 10 à 15 ms, donc une course entre la réponse DNS et l'ioctl
d'annulation reste possible.

Il faut encore caractériser :
    - le code retour sans requête correspondante ;
    - le code retour avec une entrée déjà résolue ;
    - le code retour avec pending même hostname ;
    - le code retour avec pending autre hostname ;
    - le wrapper PowerPC complet de l'export.


## dns_abort_by_hname : sémantique observable native

Une matrice native a caractérisé dns_abort_by_hname() dans plusieurs
états du resolver.

Résultats :

    aucune requête correspondante
        dns_abort_by_hname(...) -> 0

    hostname déjà résolu / en cache
        dns_abort_by_hname(...) -> 0

    résolution pending du même hostname
        getaddrinfo_async(...) -> EAI_INPROGRESS
        dns_abort_by_hname(même hostname) -> 0
        résolution -> rc=0 normalement

    résolution pending d'un autre hostname
        getaddrinfo_async(host A) -> EAI_INPROGRESS
        dns_abort_by_hname(host B) -> 0
        résolution de A -> rc=0 normalement

Le premier test avec attente de 750 ms confirme également qu'une
résolution positive devient normalement disponible après un
dns_abort_by_hname() effectué pendant EAI_INPROGRESS.

Conclusion observable :

    [✓] le code retour 0 ne signifie pas qu'une requête correspondante
        a été trouvée ;

    [✓] aucun effet d'annulation n'est observable par les appels
        getaddrinfo_async testés ;

    [✓] une implémentation AX qui annulerait réellement la requête lwIP
        serait plus agressive que le comportement natif mesuré.

Parité retenue pour le shim AX :

    AX actif :
        dns_abort_by_hname(hostname) -> 0
        aucune modification du resolver lwIP

    AX inactif :
        passthrough vers nsysnet natif
