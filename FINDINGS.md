# AX88179 Wii U Aroma - findings actuels

Date: 2026-09-17

## SHIM VALIDÉ — TCP+UDP via l'adaptateur (2026-09-17)

Le test `test_shim_boot` attend maintenant l'IP **localement** (sonde
`socket()` : fallback natif marqué par `errno=-1`, puis `SO_MYADDR` non
nul — lwIP renvoie 0.0.0.0 tant que DHCP n'a pas abouti), au lieu d'un
aller-retour UDP vers le PC qui dépendait du serveur d'écho.

Résultat console : `AX socket path ready (IP 192.168.2.190)`, puis
TCP echo PASS 32/504/1024/1400 et UDP echo PASS 32/504/1024/1400 en
boucle, 0 erreur. Côté PC, le serveur d'écho voit 100 % du trafic
arriver de `192.168.2.190` (jamais `.124`). L'interception nsysnet est
donc fonctionnelle pour un homebrew : sockets TCP/UDP via l'adaptateur
AX88179, détection de l'IP sans dépendance externe.

Rappel sortie WUHB : HOME overlay système -> `Quitter`, rien d'autre.

---

## Objectif

Faire fonctionner un adaptateur USB Ethernet AX88179 en Wii U mode via un module Aroma usermode, avec réseau disponible depuis le menu, les homebrews et les jeux.

## État validé

- Le driver AX88179 sait initialiser l'adaptateur, démarrer RX/TX, obtenir DHCP et répondre au ping.
- Le module Aroma `v0.2.2-shim-delay30` boote, attend 30 s, applique le patch IOSU/UHS, ouvre l'AX88179, démarre lwIP/DHCP et installe les hooks `nsysnet`.
- IP observées : Wi-Fi Wii U `192.168.2.124`, Ethernet AX88179 `192.168.2.190`, PC `192.168.2.100`.
- Le shim global intercepte bien des sockets d'un homebrew Aroma : capture confirmée avec paquets sortants depuis `192.168.2.190`.
- Capture WUHB précédente : TCP SYN et UDP partent vers `192.168.2.100:18879`; le problème réseau du test n'était pas une absence d'émission.
- FTP anonyme fonctionne sur la console quand elle n'est pas figée; upload testé vers `/fs/vol/external01/wiiu/apps/...`.

## Problème actuel

Le blocage est maintenant côté lancement/sortie WUHB Aroma, pas côté AX88179/DHCP.

- `AX88179 Shim Test` peut figer la console au lancement ou à la sortie HOME/MINUS.
- `AX Safe Exit`, WUHB minimal sans réseau, sans shim, sans SDL et sans ProcUI, affiche un écran noir puis ne répond plus.
- Ce résultat isole le bug : même un WUHB OSScreen + VPAD + auto-sortie 15 s ne sort pas proprement.
- Le ping Ethernet peut continuer pendant que le WUHB est figé, donc le module AX peut rester vivant malgré le freeze de l'app.

## Fichiers importants

- Module Aroma : `wiiu_ethernet/aroma_module/`
- Shim sockets : `wiiu_ethernet/aroma_module/nsysnet_shim.c`
- Test WUHB shim : `wiiu_ethernet/test_shim_boot/`
- Test minimal qui freeze aussi : `wiiu_ethernet/safe_exit_test/`
- Echo server PC : `wiiu_ethernet/test_shim_boot/echo_server.py`

## Détails du module actuel

- Version module : `0.2.2-shim-delay30`.
- Le worker attend 30 s avant de toucher UHS pour éviter de casser wiiload/RPXLoader pendant les transferts.
- Ports réservés côté natif dans le shim : FTP `21`, wiiload `4299`.
- Malgré ces exclusions, wiiload reste fragile sous hooks globaux.

## Dernier test ajouté

`safe_exit_test` a été créé pour isoler le problème WUHB :

- App Aroma : `AX Safe Exit`
- SD : `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`
- SHA1 : `d5df9f3da7a26b404307be9b4bfe02cd21798405`
- Taille : `56652` octets
- Code : OSScreen + VPAD, pas de réseau, pas de shim, pas de SDL, pas de ProcUI
- Résultat utilisateur : écran noir, console bloquée


## Safe Exit v2 déployé

Après le freeze du premier `safe_exit_test`, comparaison avec `wiiu_console/bringup` : le test minimal n'utilisait pas le squelette ProcUI/VPAD connu du repo.

Correction appliquée :

- `safe_exit_test` utilise maintenant `ProcUIInitEx` via `proc.c/proc.h` repris de `wiiu_console/bringup/src/`.
- Ajout explicite de `VPADInit()`.
- Garde OSScreen + texte + HOME/MINUS/PLUS/B + auto-exit 15 s.
- Nouveau WUHB copié et vérifié sur SD : `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`.
- SHA1 v2 : `8f2e52db8d6542ac7a4df7276246d4702488a148`, taille `62348` octets.

À tester : relancer l'icône `AX Safe Exit`. Si v2 sort proprement, reprendre les tests WUHB réseau depuis ce squelette.


## Résultat console Safe Exit v2

Test utilisateur : `AX Safe Exit v2` lancé depuis Aroma.

Affichage observé :

```text
AX Safe Exit 2 - ProcUI skeleton
err=0 hold=00000000 trig=00000000
auto exit in 7 s
HOME/MINUS/PLUS/B exits
```

Logs UDP observés :

```text
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
AX88179 module: worker stopped, interface released
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
AX88179 module: IOSU patch applied
AX88179 module: worker stuck, leaving it to the process teardown
AX88179 module: worker started v0.2.2-shim-delay30 (SHIM ON, RX sync 5000us)
ax_safe_exit2: starting
ax_safe_exit2: frame=60 elapsed=2
ax_safe_exit2: frame=120 elapsed=4
ax_safe_exit2: frame=180 elapsed=7
ax_safe_exit2: frame=240 elapsed=9
ax_safe_exit2: frame=300 elapsed=11
ax_safe_exit2: frame=360 elapsed=14
ax_safe_exit2: auto exit
ax_safe_exit2: auto exit
AX88179 module: worker stopped, interface released
ax_safe_exit2: done
```

Conclusion :

- Le squelette WUHB v2 est bon pour l'affichage, VPAD et la boucle principale.
- L'app atteint `ax_safe_exit2: done`, donc le freeze ne vient plus du code applicatif avant `return 0`.
- Après la sortie, la console reste figée sur menu + logo et le ping Ethernet ne répond plus.
- Le problème restant est probablement dans la transition Aroma/ProcUI/retour titre ou dans le teardown/redémarrage du module Aroma autour de la fermeture de l'app.
- Le log `worker stuck, leaving it to the process teardown` est suspect : il faut corriger l'arrêt du worker AX avant de continuer les tests shim réseau.

Prochaine action recommandée :

1. Rendre l'arrêt du worker déterministe : flag stop, réveil RX, timeout court, ne pas laisser de thread bloqué dans UHS/RX pendant `APPLICATION_ENDS`.
2. Tester `AX Safe Exit v2` avec le module AX désactivé ou avec shim/worker non démarré pour séparer bug ProcUI pur et bug teardown module.
3. Si sans module le retour menu marche, corriger `aroma_module` avant tout nouveau WUHB réseau.


## Module v0.2.3-persist-worker déployé

Changement fait après le freeze post-`ax_safe_exit2: done` :

- Le worker AX n'est plus arrêté dans `WUMS_APPLICATION_REQUESTS_EXIT` ni `WUMS_APPLICATION_ENDS`.
- Ces hooks arrêtent seulement l'acceptation shim et loggent que le worker reste vivant.
- `WUMS_APPLICATION_STARTS` appelle maintenant `nsysnet_shim_begin_title()` même si le worker tourne déjà.
- `stop_worker()` reste utilisé dans `WUMS_DEINITIALIZE`.
- But : éviter le blocage UHS/RX pendant les transitions Aroma/WUHB, suspecté par `worker stuck, leaving it to the process teardown`.
- WMS copié et vérifié sur SD : `/wiiu/environments/aroma/modules/AX88179Module.wms`.
- SHA1 : `ec8a3d6386886ef2af06f9211d2788f84e3f8773`.

À tester après redémarrage : lancer `AX Safe Exit`, attendre l'auto-exit ou sortir avec B/MINUS, vérifier retour menu et ping.


## Safe Exit v3 déployé

Correction après clarification : le boot menu fonctionne, le blocage arrive à la sortie de `AX Safe Exit`.

Cause probable trouvée : `safe_exit_test` v2 avait repris le vieux `proc.c` de `wiiu_console/bringup`, qui appelle `SYSRelaunchTitle()` dans `proc_shutdown()`. Ce chemin avait déjà été identifié comme bloquant dans le contexte Aroma/Health & Safety.

Changement v3 :

- `safe_exit_test/proc.c` et `proc.h` remplacés par la version corrigée de `wiiu_ethernet/common/`.
- `proc_shutdown()` ne relance plus le titre; il logge puis retourne de `main`.
- L'affichage indique `AX Safe Exit v3 - no relaunch`.
- WUHB copié et vérifié sur SD : `/wiiu/apps/ax_safe_exit/ax_safe_exit.wuhb`.
- SHA1 v3 : `82be508073d8bddd2bb85a79e1961b133895fc00`.

À tester : lancer `AX Safe Exit`, attendre auto-exit ou sortir avec B/MINUS, vérifier si le menu revient sans freeze et si le ping reste actif.


## Safe Exit v4 déployé

Résultat v3 : l'app atteint `AXPROBE ProcUI: shutdown end` puis `ax_safe_exit3: done`, mais reste sur écran noir. Donc le code sort jusqu'après `ProcUIShutdown`, mais le retour système ne reprend pas.

Changement v4 :

- `proc_stop()` appelle toujours `SYSLaunchMenu()` pour demander explicitement le retour menu.
- `SYSRelaunchTitle()` reste interdit.
- `g_running` est ensuite mis à 0 pour sortir proprement de la boucle.
- Écran : `AX Safe Exit v4 - launch menu`.
- SHA1 v4 : `07fb64fcab6b597fdeea4e46fee5a4d2940de81b`.


## Safe Exit v5 prêt mais pas encore déployé

Résultat v4 :

```text
ax_safe_exit4: auto exit
AXPROBE ProcUI: SYSLaunchMenu begin
AXPROBE ProcUI: SYSLaunchMenu returned
AXPROBE ProcUI: status=2
AXPROBE ProcUI: shutdown begin
AXPROBE ProcUI: shutdown end
ax_safe_exit4: done
```

La console reste ensuite sur écran noir. Interprétation : v4 fait `ProcUIShutdown()` trop tôt, juste après `PROCUI_STATUS_RELEASE_FOREGROUND` (status 2).

Changement v5 local :

- `proc_stop()` appelle `SYSLaunchMenu()` mais ne met plus `g_running=0`.
- Le code continue à pomper `ProcUIProcessMessages()` après status 2.
- `ProcUIDrawDoneRelease()` peut rendre le foreground, puis on attend `PROCUI_STATUS_EXITING` avant `ProcUIShutdown()`.
- SHA1 v5 local : `2a58403af03cdb06838d1bd4d3752568e8d78762`.
- Upload FTP non fait car la console était encore figée et `192.168.2.124:21` ne répondait plus.


## Module no-op isolation déployé

Résultat v5 : ProcUI atteint `status=3`, `ProcUIShutdown end`, puis `ax_safe_exit5: done`, mais le menu reste bloqué au redémarrage. Cela prouve que le WUHB sort correctement jusqu'au bout.

Test suivant déployé sur SD : module Aroma `0.2.4-noop-isolation`.

- Aucun worker AX.
- Aucun shim.
- Hooks WUMS seulement loggés.
- SHA1 WMS : `6edab878cb2c13f73b4d5d7d45d15422e740b4d1`.
- But : lancer `AX Safe Exit v5` avec un module neutre. Si le menu revient, le freeze vient du worker AX/shim. Si ça bloque encore, le souci est hors module AX.


## Résultat no-op isolation

Avec le module Aroma `0.2.4-noop-isolation` :

```text
AX88179 module: noop isolation loaded v0.2.4
AX88179 module: noop app starts
AX88179 module: noop app requests exit
AX88179 module: noop app ends
ax_safe_exit5: starting
AXPROBE ProcUI: status=0
...
AXPROBE ProcUI: SYSLaunchMenu returned
AXPROBE ProcUI: status=2
AXPROBE ProcUI: status=3
AXPROBE ProcUI: shutdown begin
AXPROBE ProcUI: shutdown end
ax_safe_exit5: done
```

La console reste bloquée au redémarrage du menu. Conclusion : le freeze n'est pas causé par le worker AX, le shim ou UHS. Il est dans la séquence de sortie WUHB/Aroma/ProcUI.

Détail affichage : la ligne `err=0 hold=00000000 trigger=00000000` est normale, elle vient du texte dessiné à l'écran par `safe_exit_test/main.c`, pas des logs UDP.

Prochain test : v6 doit traiter le contexte Aroma/Health & Safety comme un titre emprunté et utiliser `SYSRelaunchTitle()` après `ProcUIShutdown`, avec le module no-op encore actif pour tester cette sortie sans AX/shim.


## Résultat Safe Exit v6

Résultat utilisateur : `SYSRelaunchTitle()` ne revient pas au menu Aroma; il relance l'app de test elle-même.

Conclusion : `SYSRelaunchTitle()` est rejeté pour ce contexte WUHB Aroma. Les pistes testées sont maintenant :

- Retour simple après `ProcUIShutdown` : écran noir.
- `SYSLaunchMenu()` puis attendre `EXITING` : bloqué au redémarrage menu.
- `SYSRelaunchTitle()` : relance l'app elle-même.

Prochaine piste : tester un WUHB sans `SYSLaunchMenu` ni `SYSRelaunchTitle`, qui demande seulement l'arrêt ProcUI et retourne, mais avec une séquence plus proche des exemples Aroma/HBL si trouvée.


## Safe Exit v7 déployé

Après v6 : `SYSRelaunchTitle()` relance l'app elle-même, donc rejeté.

Changement v7 :

- Sortie par `_SYSDirectlyReturnToCaller()` après `ProcUIShutdown`.
- Pas de `SYSLaunchMenu()`.
- Pas de `SYSRelaunchTitle()`.
- Écran : `AX Safe Exit v7 - direct return`.
- SHA1 v7 : `b8edd704c2ee68589e2b70896609e21afc389473`.


## Safe Exit v8 déployé

Constat : des tests précédents quittaient correctement (`2026-09-16-probe-home-direct-first/second`). Ils utilisaient le flux `probe_init` / `probe_poll` / `probe_shutdown`, avec `ui_shutdown()` avant `proc_shutdown()`.

`safe_exit_test` v1-v7 utilisait un chemin OSScreen custom et divergeait de ce flux.

Changement v8 :

- `safe_exit_test` reprend `common/probe.c`, `common/ui.c`, `common/proc.c`.
- Le main est réduit à `probe_init`, boucle `probe_poll`, auto-exit, puis `probe_shutdown`.
- Donc même flux que les tests qui avaient déjà rendu le menu deux fois.
- SHA1 v8 : `4fa11d2aaa973e67b7dc54e46ce490053d72b0e6`.


## Safe Exit v9 déployé

Correction de méthode après comparaison avec `capture2wiiu` : capture2wiiu quitte par le HOME overlay système puis bouton `Quitter`, pas par auto-exit ni sortie forcée.

Changement v9 :

- HOME n'est plus intercepté.
- Pas d'auto-exit.
- Pas de `SYSLaunchMenu`, `SYSRelaunchTitle`, `_SYSDirectlyReturnToCaller`.
- ProcUI normal, HOME overlay système autorisé.
- Instruction test : appuyer HOME puis choisir `Quitter` dans l'interface Wii U.
- SHA1 v9 : `0cc7c011637b7dbbaa45016d6db68a507e5e00f4`.


## Résultat Safe Exit v9 et restauration module normal

Résultat utilisateur : v9 fonctionne. Le chemin correct de sortie pour les WUHB Aroma est le HOME overlay officiel puis bouton `Quitter`. Les sorties forcées testées avant étaient le problème.

Conclusion :

- Ne pas intercepter HOME pour quitter dans les WUHB de test normaux.
- Laisser le menu HOME système s'ouvrir.
- L'utilisateur choisit `Quitter`.
- Les chemins auto-exit, `SYSLaunchMenu`, `SYSRelaunchTitle` et `_SYSDirectlyReturnToCaller` ne doivent pas servir de chemin principal.

Module AX normal restauré sur SD :

- Version : `0.2.3-persist-worker`.
- Chemin : `/wiiu/environments/aroma/modules/AX88179Module.wms`.
- SHA1 : `54da8e746bdce256879aee8b0219ad29b74fe417`.
- Le no-op reste en backup sous `AX88179Module.wms.off`.

Prochain test : redémarrer avec le module normal, attendre DHCP/ping, lancer les tests avec sortie via HOME overlay officiel -> `Quitter`.


## Module v0.2.5-restart-worker déployé

Résultat `0.2.3-persist-worker` : au menu, logs seulement :

```text
AX88179 module: worker started v0.2.3-persist-worker ...
AX88179 module: app requests exit, worker kept alive
AX88179 module: app ended, worker kept alive
```

Pas d'IP Ethernet ensuite. Conclusion : garder le worker vivant à travers une transition Aroma ne marche pas; le thread démarre dans un titre transitoire et n'atteint pas DHCP.

Changement `0.2.5-restart-worker` :

- Retour au stop/restart sur `WUMS_APPLICATION_REQUESTS_EXIT` et `WUMS_APPLICATION_ENDS`.
- Conserver la méthode correcte de sortie WUHB : HOME overlay officiel -> `Quitter`.
- WMS copié et vérifié sur SD.
- SHA1 : `ec95ecf61f6f06b9e492fa90ba33044ee045968a`.

À tester après redémarrage : attendre les logs `worker started v0.2.5`, `IOSU patch`, DHCP BOUND, puis ping `192.168.2.190`.


## Module v0.2.6-keep-ready déployé

Résultat du premier `AX88179 Shim Test` avec v0.2.5 : TCP/UDP echo PASS, mais les PASS arrivent avant le nouveau `DHCP BOUND`. Cela peut être un faux positif via Wi-Fi/native fallback, car v0.2.5 stoppe le worker à la transition de titre puis attend 30 s avant de rouvrir l'AX.

Changement v0.2.6 :

- Si `ax_net_stack_ready()` est vrai pendant `APPLICATION_REQUESTS_EXIT/ENDS`, ne pas stopper le worker; seulement `nsysnet_shim_stop_accepting()` pendant la transition.
- Au prochain `APPLICATION_STARTS`, `nsysnet_shim_begin_title()` réactive l'acceptation.
- Si la stack n'est pas prête, conserver l'ancien stop/restart pour éviter le bug `persist-worker` dans les titres transitoires.
- Ajout de timestamps relatifs (`[ms]`) aux logs du module.
- SHA1 WMS : `7d18b6494b78b9cce41769ae267ac98f31d8eadf`.

À tester : redémarrer, attendre DHCP menu, lancer `AX88179 Shim Test`, vérifier que le worker est gardé vivant et que les paquets `18879` sortent de `192.168.2.190`.

## Prochaine piste recommandée

1. Stopper les tests réseau WUHB tant que la base WUHB ne sort pas proprement.
2. Trouver dans le repo ou sur la SD un WUHB Aroma connu fonctionnel, puis copier exactement son squelette init/loop/exit.
3. Comparer avec `safe_exit_test`, surtout init écran, ProcUI, title/app metadata et règles Makefile/WUHB.
4. Si OSScreen direct pose problème sous Aroma, remplacer par le squelette UI déjà validé dans un autre homebrew.
5. Une fois un WUHB minimal qui lance et quitte proprement validé, réintroduire par étapes : logs écran, puis sockets natifs, puis shim, puis test TCP/UDP.

## Commandes utiles

Upload FTP :

```sh
curl -sS --fail --ftp-create-dirs -T <file.wuhb> \
  ftp://anonymous:@192.168.2.124/fs/vol/external01/wiiu/apps/<app>/<app>.wuhb
```

Vérifier echo server PC :

```sh
ss -ltnup | grep 18879 || python3 -u wiiu_ethernet/test_shim_boot/echo_server.py >/tmp/ax-boot-echo.log 2>&1 &
```

Attention : ne pas utiliser `pgrep -af 'echo_server.py' || ...`, ça peut matcher son propre shell et ne pas lancer le serveur.
