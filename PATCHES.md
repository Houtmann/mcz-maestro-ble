# Correctifs locaux

Fork de [foyewmaddeeb/mcz-maestro-ble](https://github.com/foyewmaddeeb/mcz-maestro-ble).
Testé sur un MCZ Maestro **M2X.AIR.24.20 (Panel:16)**, poêle à air canalisé 12 kW,
3 ventilateurs, 5 niveaux, `banca_dati CS21`.

## 1. WiFi — la reconnexion à 5 s empêchait toute association

`netTick()` rappelait `WiFi.reconnect()` toutes les 5 secondes tant que `WL_CONNECTED`
était faux. Or `WL_CONNECTED` exige d'avoir obtenu un bail DHCP. Sur un point d'accès lent
— ici celui d'un Raspberry Pi, aggravé par `WiFi.setSleep(true)` obligatoire pour la
coexistence BLE/WiFi — le firmware arrachait l'association avant l'arrivée du bail, en
boucle. Côté hostapd cela donne :

```
STA xx IEEE 802.11: associated
STA xx WPA: pairwise key handshake completed (RSN)
STA xx IEEE 802.11: disassociated      <- quelques secondes plus tard
```

**Correctif** : délai porté de 5 s à 20 s.

## 2. IP statique optionnelle

Complément du point 1 : supprime entièrement l'étape DHCP. Désactivé par défaut.

```c
#define USE_STATIC_IP  1
#define STATIC_IP      "192.168.0.20"
#define STATIC_GW      "192.168.0.1"
#define STATIC_MASK    "255.255.255.0"
#define STATIC_DNS     "192.168.0.1"
```

Penser à choisir une adresse **hors** du pool DHCP du point d'accès.

## 3. Ventilateurs canalisés 2 et 3

Le firmware détectait `fans=3` mais n'exposait qu'un seul ventilateur, via
`REG_FAN_SET = 0x03FA`. Les deux autres sont dans les registres consécutifs :

| Registre | Nom côté cloud MCZ | Rôle |
|---|---|---|
| `0x03FA` | `set_vent_v1` | ventilateur 1 (déjà géré) |
| `0x03FB` | `set_vent_v2` | 2e ventilateur, gaine |
| `0x03FC` | `set_vent_v3` | 3e ventilateur, gaine |

Même encodage que le premier : `1..5` = niveau fixe, `6` = automatique.

Ajouts : lecture dans le bloc de poll (`0x03E9` étendu de 15 à 20 registres), champs
`fan2Set` / `fan3Set`, fonction `ovenSetFanN()`, commandes série `fan2` / `fan3`, deux
entités `select` supplémentaires en discovery MQTT (`set/fan2`, `set/fan3`), et affichage
dans `status`.

> **Correspondance à confirmer.** Le mappage `0x03FB`/`0x03FC` → ventilateurs 2 et 3 est
> déduit du nommage `set_vent_v1/v2/v3` de l'intégration cloud et de la contiguïté des
> registres, dont les valeurs lues sont cohérentes (1..6). Il n'a pas encore été vérifié
> contre le comportement physique du poêle en marche.

## 4. `.gitignore`

Le dépôt amont n'en contient pas, alors que son README annonce `src/config.h` comme ignoré.
Sans lui, un `git add -A` publie les identifiants WiFi et MQTT — et `.pio/` contient le
binaire compilé, dans lequel ces mêmes chaînes sont présentes en clair.

## Utilitaire

`setup-secrets.sh` renseigne les quatre secrets de `src/config.h` par saisie masquée, sans
les faire passer par l'historique du shell.

## 5. Diagnostic réseau — commande série `wifi`

Affiche `WiFi.status()`, l'adresse MAC, l'IP, le RSSI, puis scanne les réseaux visibles en
marquant le SSID cible. Indispensable pour distinguer un problème de portée d'un problème
d'association : ici le RSSI était de −52 dBm alors que le firmware restait en
`WL_DISCONNECTED`, ce qui a écarté d'emblée l'hypothèse de la distance.

## Piège vérifié : ne pas toucher au modem-sleep

Le commentaire amont « BLE+WiFi coexistence: modem sleep MUST be on » n'est pas une
précaution de principe. Remplacer `WiFi.setSleep(true)` par `false` fait **paniquer la puce
et redémarrer en boucle** (`SW_CPU_RESET` avec backtrace). Testé, confirmé, annulé — le
commentaire a été renforcé dans le code.

## Nommage des entités Home Assistant

L'`entity_id` est dérivé du **nom** de l'entité, pas de l'`object_id` de discovery : les
entités « Fan 2 » et « Fan 3 » apparaissent en `select.<device>_fan_2` et `_fan_3`, avec un
souligné.

## 6. Disponibilité MQTT — course avec le testament (LWT)

`netTick()` ne publiait le topic de disponibilité que **sur changement** de l'état BLE.
Quand la session MQTT tombe et se rétablit, le broker publie le testament `offline`
(retenu) au moment où il détecte la session morte — ce qui peut arriver **après** que
l'ESP se soit reconnecté et ait publié `online`. Home Assistant reste alors bloqué sur
`offline` indéfiniment, alors même que les états continuent d'arriver et sont visibles
dans le log du broker :

```
Received PUBLISH from mcz-xxxx (... 'mcz/xxxx/state' ...)   <- les donnees arrivent
```
tout en affichant l'entité `unavailable` dans HA.

**Correctif** : republier la disponibilité à chaque battement (30 s), et plus seulement
sur changement. Le message est retenu et idempotent, la republication est sans effet de
bord.

## 7. Point d'accès de secours pour le diagnostic

Une fois posé loin de tout ordinateur, ce montage n'avait **aucun moyen de dire pourquoi il
ne se connectait pas** : ni console série, ni MQTT, ni la moindre trace côté hostapd quand
l'association échoue avant même la première tentative.

Si le WiFi n'est pas connecté `FALLBACK_AP_DELAY_S` secondes après le démarrage, l'ESP passe
en `WIFI_AP_STA`, ouvre son propre SSID et sert une page de texte brut sur
`http://192.168.4.1/` : uptime, `WiFi.status()`, SSID cible, MAC, IP, RSSI, état MQTT, état
BLE, température ambiante, et le **scan des réseaux visibles** capturé au moment de
l'ouverture, SSID cible marqué. Il continue d'essayer le WiFi normal en parallèle et referme
l'AP dès qu'il y parvient.

Réglages dans `config.h` (`FALLBACK_AP` à `0` pour désactiver) :

```c
#define FALLBACK_AP           1
#define FALLBACK_AP_SSID      "MCZ-Bridge"
#define FALLBACK_AP_PASS      "mczbridge"
#define FALLBACK_AP_DELAY_S   60
```

**Coexistence radio.** Faire tourner AP + STA + BLE simultanément est précisément le type de
sollicitation qui fait planter la puce (cf. le piège du modem-sleep plus haut). Le délai de
60 s garantit que l'AP ne s'ouvre que dans un état où le WiFi est déjà perdu — donc aucun
risque ajouté en marche nominale. Vérifié en conditions réelles : `ble=1` maintenu, pas de
`SW_CPU_RESET`, heap libre 136 ko (minimum 115 ko) contre 144 ko sans l'AP.

**Piège d'inclusion** : `#include <WebServer.h>` doit être placé **après** `#include
"config.h"` dans `net_mqtt.cpp`, sinon la macro `FALLBACK_AP` n'est pas encore définie et le
type n'est pas déclaré.

## 8. Garde-fou sur les compteurs — statistiques HA polluées définitivement

Les compteurs (`worktime`, `ignitions`, `time_power_1..5`) sont publiés en
`state_class: total_increasing`. Une **trame BLE corrompue** suffit à y injecter une valeur
aberrante, et Home Assistant enregistre le bond dans ses statistiques long terme — de façon
**irréversible** sans intervention manuelle.

Observé en production : temps de fonctionnement du jour à **42 348 xxx minutes** et
**13 103 allumages**, alors que les capteurs affichaient 2 225 min et 495 allumages. Un mot
haut parasite sur un compteur 32 bits suffit : `(0xFFFF<<16)/60` dépasse déjà 71 millions.

Origine probable : la troncature signalée par NimBLE dans les logs,
`NimBLEAttValue: value exceeds max, len=514, max=512`.

**Correctif** : `counterPlausible()` valide chaque mise à jour de compteur monotone —
plafond absolu, bond maximal depuis la dernière valeur, et baisse acceptée uniquement si
elle ramène près de zéro (remise à zéro légitime à l'entretien). Les valeurs rejetées sont
journalisées sur la console série.

| Compteur | Bond max | Plafond |
|---|---|---|
| `worktime` | 240 min | 5 000 000 min |
| `time_power_N` | 240 min | 5 000 000 min |
| `ignitions` | 20 | 200 000 |

Le garde-fou empêche la pollution future ; les statistiques déjà corrompues doivent être
supprimées côté Home Assistant (Outils de développement → Statistiques).

## 9. Garde-fou, suite — amorçage non contrôlé et réponses mal attribuées

Le garde-fou du §8 avait deux trous, révélés au retour du pont après quelques jours
d'absence : le compteur d'allumages est passé de 496 à **4 617** et y est resté.

1. **La première valeur après un démarrage était acceptée sans contrôle** (`prev < 0`).
2. **Ensuite, la vraie valeur était rejetée indéfiniment** : 496 < 4 617 passe pour une
   baisse impossible. Le garde-fou verrouillait la valeur fausse jusqu'au redémarrage
   suivant.

Dans la même trame, `time_power_2` a reçu 1 749 999 min, corrigé par chance à la lecture
suivante (148 ≤ bond max, donc pris pour une remise à zéro).

**Correctifs** :

- Chaque compteur a son `CounterGuard`. Une première valeur n'est acceptée qu'après
  **3 lectures identiques consécutives**. Une valeur rejetée mais relue 3 fois à
  l'identique finit par être acceptée : le compteur ne peut plus rester bloqué.
- Une réponse Modbus 03 ne porte pas d'adresse ; elle est rattachée à la dernière lecture
  envoyée (`g_lastReadBase`). Or le poll envoie une lecture toutes les 2,5 s sans attendre
  la réponse précédente : une réponse tardive est décalée sur le mauvais bloc. La taille
  de la réponse est désormais comparée au nombre de registres demandés
  (`g_lastReadCount`) ; les blocs lus font 51, 77, 20 et 8 registres, donc une réponse
  décalée est toujours détectée et ignorée.

Vérifié au reflash : `ignitions: -1 -> 497 accepte apres 3 lectures identiques`.

## 10. Temps de fonctionnement : les registres sont en minutes, pas en secondes

Le code d'origine lisait `0x0340/41` (temps total) et `0x0336..0x033F` (temps par
puissance) comme des secondes 32 bits et divisait par 60. Ce sont des **minutes**.

Preuve : 2 225 « minutes » pour 496 allumages ferait 4,5 min par allumage, alors qu'une
séquence d'allumage seule (nettoyage → chargement → Start 1 → Start 2 → stabilisation)
dure environ 8 min. En minutes, on obtient 2 225 h, soit 4,5 h par allumage : cohérent.
Les valeurs affichées étaient donc des heures tronquées, qui n'avançaient que d'une unité
par heure de chauffe — d'où l'impression que le compteur ne montait pas.

Lecture brute : temps total `2479, 2` → 133 551 min = 2 225,85 h.

**Correctif** : plus de division ; les compteurs internes sont en minutes et publiés en
heures à 2 décimales (`worktime_h`, `pt1_h`..`pt5_h`), unité `h`, `device_class:
duration`. Les `entity_id` ne changent pas, mais l'unité passe de `min` à `h` : les
statistiques long terme existantes sont à effacer côté Home Assistant.

## 11. Gardes de plage sur `power`, ventilateurs et réglages hydro (erreurs MQTT répétées dans HA)

HA logguait en boucle des valeurs hors plage : `power` = 14 (plage 1-5), `fan_level` = 0
(options `Auto, 1..5`), `hyst` = 3193,4 (0-20), `pump_min_on` = 179,2 (20-90),
`water_set` = 0 (30-85). Les compteurs d'heures et `ignitions` sont sains.

**Correctif** (symptôme seulement) :
- `main.cpp` : `REG_POWER` n'est mémorisé que si la valeur brute 16 bits est entre 1 et 5,
  testée avant le cast `int8_t` ; sinon `power` reste inchangé.
- `net_mqtt.cpp`, `publishState()` : `power` publié seulement dans 1..5 ; `fan_level`,
  `fan2`, `fan3` seulement dans 1..6 (6 → `Auto` pour fan2/fan3) ; chaque réglage de
  `HYDRO_NUMS` seulement s'il est dans `[mn, mx]` de la table.
- Les `value_template` de `power`, `fan2`/`fan3` et des numbers hydro tolèrent une clé
  absente (`... if value_json.x is defined else ''`), comme le select `fan`.
- `unique_id`, adresses de registres, écritures et plages de découverte inchangés.

Ces gardes **masquent** les valeurs aberrantes de `power` et des réglages hydro ; leur
cause reste à établir par lecture série : `r 03E9 20`, `r 03F3 4`, `r 0403 1`, `r 040F 1`.
Piste : `g_caps.hydro` est vrai à tort sur ce poêle AIR, d'où des numbers hydro exposés
alors que ces registres n'y ont pas la même signification.

## 12. Build de diagnostic `esp32dev_diag` : origine des valeurs aberrantes

Les valeurs aberrantes vues dans HA (power 14, hystérésis 1020,4 / 3193,4, pump_min_on 179,2,
water_set 0) n'ont pas pu être reproduites sur la série. Le suspect (non prouvé) est
`ovenApplyReg` appelé depuis l'image `##` (`main.cpp`, base 0x02BA, jusqu'à 768 octets), qui
écrit sans condition tous les registres, y compris `REG_MAXPOT_IDRO` (donc `g_caps.hydro`).
Ce build l'observe sans rien filtrer ; les gardes du §11 restent actives.

**Contenu** (tout sous `#ifdef MCZ_DIAG` ; le build `esp32dev` normal n'en contient aucune
chaîne : `strings firmware.elf | grep -c 'debug/attrs'` = 0) :
- `src/diag.h` / `src/diag.cpp` : file de 8 événements en tampons statiques (aucune allocation,
  verrou `portMUX`), alimentée depuis le callback BLE, vidée depuis `loop` par `netTick`.
- `main.cpp` : `DIAG_SRC` marque l'origine de chaque lot d'appels à `ovenApplyReg` (`##`, `03`,
  `06`) ; `DIAG_CHECK` en tête de `ovenApplyReg` ; `DIAG_CAPS` sur `REG_MAXPOT_IDRO` ;
  `DIAG_HASH` copie l'image `##` (memcpy borné) à la réception.
- `net_mqtt.cpp` : entité unique `sensor` « Diag pont » (`entity_category: diagnostic`, état =
  compteur `n`), tous les messages sur `mcz/<id>/debug/attrs`, sans retain, 1 message/s au plus.

**Événements** (JSON, `n` = compteur croissant ajouté à chaque message) :
- `rej` : valeur brute hors plage attendue (hystérésis 0-20, pump_min_on 20-90, water_set 30-85,
  power 1-5, ventilateurs > 6 ; 0xFFFF de `set_puffer` ignoré). Dédoublonné par registre+valeur,
  1 par 30 s. Champs : `reg`, `raw` (brut avant ÷10), `src`, `up`, `lastRead`, `lastCount`, `hydro`.
- `caps` : `g_caps.hydro` change (prioritaire, hors limite de débit) : `hydro`, `src`, `raw`, `up`.
- `hash` : image `##` en parties de 256 octets (`seq`, `part` à partir de 0, `parts`, `len`,
  `off`, `base`, `hex`), 1 partie/s ; 1 image max / 10 min, 20 max par démarrage, la première
  toujours retenue.
- `hb` : toutes les 10 min (et une fois à la première connexion MQTT) : `up`, `hashes` (images
  retenues), `rx` (images `##` reçues), `rej`, `drop` (file pleine), `rl` (limite 30 s), `hydro`.

**MQTT coupé** : `netTick` ne vide pas la file, qui se remplit puis perd les nouveaux
événements (compteur `drop`, publié dans `hb`) ; une image `##` en attente reste en tampon et
bloque les suivantes. Rien ne s'accumule, rien ne bloque. Un `publish` raté n'est pas rejoué.

**Flash** : `platformio run -e esp32dev_diag -t upload --upload-port <port>`.

**Retrait** : reflasher le build normal (`-e esp32dev`), puis supprimer l'entité en publiant un
payload vide retenu sur `<HA_DISCOVERY_PREFIX>/sensor/<id>_diag/config` (par exemple
`mosquitto_pub -r -n -t '<prefix>/sensor/<id>_diag/config'`). Vider aussi le topic
`mcz/<id>/debug/attrs` n'est pas nécessaire (non retenu).

## 13. Alignement sur la table officielle des registres

Source : `docs/mcz_m2_registers_en.csv` (sub_byte 0 = octet haut, 1 = octet bas ; type 3 =
16 bits ÷10 signé, 4 = uint16, 5 = uint32 sur 2 registres mot bas d'abord, 7 = flags 32 bits).
Les commentaires d'adresses de `oven.h` citent désormais la clé CSV.

**Libellés faux corrigés** (bloc de polling `0x02BC` ×51 = 700-750, donc aucune lecture BLE
ajoutée) :

| Registre | Avant | Réalité (clé CSV) |
|---|---|---|
| 0x02C9 (713) | « Active » | `pressione` : pression d'air, type 3 ÷10, unité inconnue |
| 0x02C8 (712) | non lu | `pressione_pascal` : pression d'air en Pa, type 3 ÷10 |
| 0x02CE (718) | « Combustion fan RPM » | `vel_imp_coclea` : **consigne** vitesse vis sans fin |
| 0x02CF (719) | non lu | `vel_real_coclea` : vitesse réelle vis sans fin |
| 0x02D1 (721) | « Flue gas fan RPM » | `vel_imp_ventola_fumi` : **consigne** ventilateur fumées |
| 0x02D2 (722) | non lu | `vel_real_ventola_fumi` : vitesse réelle ventilateur fumées |

Entités Home Assistant (préfixe `unique_id` = `<id>_`) :
- **Supprimées** (config de découverte vide retenue, republiée à chaque connexion MQTT) :
  `active` (« Active ») et `fan_comb` (« Combustion fan ») : leur sens réel était autre.
- **Conservée, renommée** : `fan_room` garde son `unique_id` ; nom « Flue gas fan setpoint »
  (c'était déjà 0x02D1, la consigne). Clé d'état `fan_room` → `fumes_fan_set`.
- **Ajoutées** : `fumes_fan_speed` (0x02D2), `auger_set` (0x02CE), `auger_speed` (0x02CF),
  `air_pressure_pa` (0x02C8, Pa, device_class `pressure`), `air_pressure` (0x02C9, brut ÷10,
  sans unité), `power_actual` (0x0323 octet haut `potenza_att`, ignoré hors 0..5),
  `service_h` (`ore_service` 846-847, h, diagnostic, rejeté > 100000), `reset_cause`
  (`dia_res_cau` 849, diagnostic). Les vitesses ignorent 0xFFFF.
- Clés d'état retirées : `fan_comb`, `active` ; la sortie série `status` suit.

**Décodage** :
- `flags` (0x0332, 32 bits) : passé de `int16_t` à `uint16_t` + `flagsOk` ; `ovenSilent()` ne
  se trompait que si le bit 15 était posé (lu comme négatif). Comportement inchangé sinon.
- 0x0372 : `g_caps.clima` teste l'octet bas (`stato_clima`) seulement ; l'octet haut est
  `tipo_allarme_clima`. Seul usage de ce registre (affiché dans `status` / capScan).
- Températures, consignes, hystérésis, pression : lues en `int16_t` avant ÷10. La sentinelle
  0xFFFF de `set_puffer` est testée avant. `diag.cpp` (plages `rej`) suit le même décodage.

**Non fait** : `cont_reset_pan` (877, compteur de resets panneau) est hors du bloc `0x0320` ×77
(800-876) : non lu, rien publié. L'ajouter demande de passer le bloc à 78 registres (ou une
lecture dédiée), à décider.

**À vérifier par lecture série** (`status`, `r 02C8 8`, `r 034E 4`) : unité de `pressione`
(713) et plage de `pressione_pascal` ; unité des vitesses vis sans fin et ventilateur
fumées (« rpm » conservé sur les fumées seulement, par continuité) ; `ore_service` est-il
bien un décompte en heures ; valeurs de `dia_res_cau` ; que `potenza_att` suit bien la
puissance réelle ; si 0x0372 vaut 0 sur ce poêle AIR.

## 14. Mise à jour OTA par HTTP

L'ESP est serveur (pas d'`espota` : le retour TCP ne passe pas depuis un conteneur Docker sur
Mac). `net_mqtt.cpp` : `POST /update` (multipart, champ `firmware`), Basic Auth utilisateur
`admin`, mot de passe `OTA_PASSWORD` de `config.h`. Aucune lib ajoutée (`WebServer` + `Update`
du core Arduino).

- **Un seul serveur web** : le `WebServer` du point d'accès de secours (§7, port 80) est
  réutilisé. L'OTA démarre quand le WiFi STA est connecté ; si l'AP de secours s'ouvre, puis
  se referme au retour du WiFi, `g_otaUp` est remis à faux (le `stop()` ferme le serveur) et
  l'OTA redémarre. Le serveur est aussi compilé si `FALLBACK_AP=0` mais `OTA_PASSWORD` défini.
- **Désactivation** : `OTA_PASSWORD` absent (code non compilé) ou `""` (compilé, inerte) →
  log série `>> OTA desactivee (...)` au démarrage. `config_example.h` donne une valeur bidon
  (`change-me`) ; `setup-secrets.sh` demande aussi ce mot de passe (vide = inchangé).
- **Pendant l'envoi** : l'écriture est synchrone dans `handleClient()`, donc `loop()` (polling
  BLE compris) est suspendu ; la disponibilité MQTT passe à `offline` au début (rétablie par le
  battement de 30 s si l'envoi échoue). Authentification vérifiée dès le premier bloc, avant
  `Update.begin`. Binaire invalide/tronqué (`Update.end` ou `Update.hasError`) → HTTP 500 avec
  la cause, ancien firmware conservé ; redémarrage seulement en cas de succès.
- **Outils** (`tools/docker/`, tout dans Docker, rien d'installé sur le Mac) : `Dockerfile`
  (python:3.12-slim + `platformio==6.1.16`), `build.sh <env>` (volume `pio-cache` pour les
  toolchains), `ota.sh <ip> [env]` (build puis `curl -u admin:...` ; mot de passe via
  `$OTA_PASSWORD` ou saisie masquée, transmis à curl par stdin, jamais écrit ni affiché).

**Partitions.** `esp32dev` et `esp32dev_diag` utilisent `default.csv` (deux slots OTA `app0` /
`app1` de 0x140000 = 1 310 720 octets, `otadata` en 0xE000). `cyd` garde `huge_app.csv` (un seul
slot de 3 Mo, **pas d'OTA sur cyd** : son firmware fait 1,4 Mo, trop gros pour un slot). `nvs`
est identique dans les deux tables (0x9000, taille 0x5000) : les réglages NVS (WiFi, MQTT,
cible) survivent au changement. Seul `spiffs` bouge (non utilisé). Marge flash : l'image
`esp32dev` occupe 1 102 313 octets sur 1 310 720 (84,1 %), `firmware.bin` = 1 108 896 octets ;
`esp32dev_diag` 1 105 469 (84,3 %). Marge d'environ 200 Ko : à surveiller à chaque ajout.
**Le changement de table impose un flash USB complet, avec le nouveau `partitions.bin`.**

**Premier flash USB** (navigateur, esptool-js), fichiers produits par `build.sh esp32dev` dans
`mcz-maestro-ble/.pio/build/esp32dev/`, mêmes offsets quelle que soit la table :

| Offset | Fichier |
|---|---|
| 0x1000 | `bootloader.bin` |
| 0x8000 | `partitions.bin` |
| 0xE000 | `boot_app0.bin` (framework : `packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin` dans le volume Docker `pio-cache`) |
| 0x10000 | `firmware.bin` |

`partitions.bin` est celui du build `esp32dev` actuel (table `default.csv`) : il doit être flashé au premier flash USB, sans quoi l'OTA reste impossible. Pour extraire
`boot_app0.bin` : `docker run --rm -v pio-cache:/c -v "$PWD":/out python:3.12-slim cp /c/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin /out/`.
Ensuite, mises à jour par `ota.sh`. Vérifié : compilation `esp32dev` (OTA désactivée) et
`esp32dev_diag` avec `-DOTA_PASSWORD=\"x\"` (OTA compilée) ; **non testé sur le matériel**.

## 15. Capteurs et attributs supplémentaires d'après la table M2

Tous les registres sont dans nos blocs de lecture existants (`0x02BC` ×51 = 700-750,
`0x0320` ×77 = 800-876) : **aucune lecture BLE ajoutée**, sauf le cas 905 ci-dessous.
Aucune valeur aberrante n'est publiée (0xFFFF / 0x7FFF / 0x8000 et hors plage → clé absente).

| Entité (`unique_id` `<id>_…`) | Registre (clé CSV) | Notes |
|---|---|---|
| `outdoor_temp` | 0x02C3 / 707 `temp_sonda_est` | type 3 signé ÷10, plage -40..60 °C, `temperature` |
| `panel_temp` | 0x02C7 / 711 `temp_pan` | diagnostic, plage -20..100 °C |
| `pellet_autonomy` | 0x032A / 810 `auto_pellet` | uint16 brut, **unité inconnue, à valider** |
| `bat_v1`, `bat_v2`, `bat_cv1`, `bat_cv2` | 0x02E6..E9 / 742-745 | V, plage 0..60 V |
| `bat_level_panel`, `bat_level_abs` | 0x0365 / 869 (octet haut / bas) | %, 0..100 |
| `bat_status`, `bat_capacity` | 0x0368 / 872, 0x0369 / 873 | bruts |
| `sleep_time` | 0x0389 / 905 `tempo_sleep` | **hors de nos blocs** : alimenté seulement si l'image `##` (base 0x02BA) le contient |

Les entités batterie et `sleep_time` sont des diagnostics **désactivés par défaut**
(`enabled_by_default: false`) : le CSV les range en « BLDC only » / « undefined », sans objet
probable sur ce poêle AIR. 0 °C ne peut pas être distingué d'une sonde extérieure absente si
elle renvoie 0 : à voir sur le poêle.

**Phase** : le capteur `Phase` expose en attributs `fase_op` (octet bas de 0x0320) et
`sub_fase_op` (octet haut). `stateName` reçoit `0x0203` « Turning off 2 » (**hypothèse** : même
phase 3, sous-phase 2 ; `0x0103` garde son libellé « Turning off » pour ne pas casser d'éventuelles
automatisations). Aucun code « refroidissement » n'est identifié : un code inconnu s'affiche
désormais « Phase <fase>/<sub> » au lieu de `0xNNNN`.

**Alarmes** : attribut `hint` (« indicatif : … ») sur le capteur « Last alarm » et le binary_sensor
« Alarm » pour A02, A04, A05, A08, A09, A12, A13, A14, A22 ; A03 (délai seulement) et les codes
inconnus : pas d'attribut. Table statique `alarmHint()` dans `net_mqtt.cpp`, indicative.

**Commandes 908 / 909 / 917 : NON exposées.** Le CSV les donne en **type 4** (uint16, paramètre),
pas type 13 (commande) comme `com_on_off` 906 / `com_reset_allarm` 907 : rien ne prouve qu'une
écriture de 1 déclenche l'action. Pas de bouton MQTT ni de commande série dédiée ; la commande
série générique `w <regHex> <valHex>` (908 = 0x038C, 909 = 0x038D, 917 = 0x0395) reste
disponible pour un essai manuel, à vos risques.

**À valider par lecture série** : `r 02C3 1`, `r 02C7 1`, `r 02E6 4` (sondes, batteries),
`r 032A 1` (unité de l'autonomie : comparer à la consommation réelle), `r 0365 1`, `r 0368 2`,
`r 0389 1` (905 hors blocs, lecture directe), `r 0320 1` en phase d'arrêt pour confirmer
0x0203 et chercher le code de refroidissement, et un déclenchement d'alarme réel pour les `hint`.

## 16. Wi-Fi prioritaire sur le BLE, IP et état OTA exposés

**Symptôme** (journal série, poêle hors de portée BLE) : `wifi=0` pendant 7 min, `WiFi status=6`
(déconnecté), `scanNetworks()` = 0 réseau alors que la Livebox et le réseau cible sont à côté,
et ~8 min pour se reconnecter près du poêle ; le port 80 (`/update`) injoignable.

**Cause retenue : le BLE affame le Wi-Fi** (une seule radio partagée). Preuves dans le code :
- `main.cpp` `setup()` : `setInterval(100); setWindow(99)` = **scan continu** (fenêtre = 99 % de
  l'intervalle) ; `startScan()` relance un scan de 8 s dès que le précédent finit : la radio
  n'est jamais libre pour l'association ni pour le scan Wi-Fi.
- `connectTarget()` : `NimBLEClient::connect()` utilise un délai par défaut de **30 s**
  (`m_connectTimeout = 30000` dans la lib) et est bloquant : `loop()` ne tourne pas, donc ni
  `netTick()` (watchdog, `handleClient()` du port 80) ni le reste. Avec un poêle hors de
  portée, chaque tentative gèle la boucle jusqu'à 30 s, puis un nouveau scan démarre.
- `netTick()` : seul `WiFi.reconnect()` toutes les 20 s, jamais d'association complète.

**Correctifs**
- **Silence BLE pendant l'association** : `netBleHold()` est vrai tant que le Wi-Fi est
  déconnecté et que moins de 30 s se sont écoulées depuis le début de la tentative (démarrage
  ou watchdog). Pendant ce temps : pas de `startScan()` (sauf scan demandé par l'utilisateur),
  scan en cours stoppé, connexion BLE différée (`g_doConnect` conservé). Wi-Fi établi : aucune
  contrainte, comportement inchangé.
- Scan BLE à rapport cyclique ~30 % (`setWindow(30)` pour `setInterval(100)`), pause de 2 s
  entre deux scans automatiques, délai de connexion BLE ramené à 5 s (`setConnectTimeout(5)`).
- **Watchdog Wi-Fi** : sans connexion depuis > 60 s : `WiFi.disconnect(true)` + `begin` complet
  (`wifiBegin()`, qui refait aussi l'IP statique et `setSleep(true)`, toujours obligatoire avec
  le BLE) ; sans connexion depuis > 10 min : `ESP.restart()` (loggé). Pendant un Wi-Fi absent,
  le BLE dispose donc de ~30 s sur 60.
- **Commande série `wifi`** : BLE mis en pause (scan arrêté, 200 ms) pendant `scanNetworks()` ;
  un code négatif est affiché (`-1` en cours, `-2` échec) au lieu de « aucun réseau visible ».
- **OTA / port 80** : `otaStart()` est rappelé à **chaque** (re)connexion STA (`stop()` +
  `begin()` du `WebServer`, handlers enregistrés une seule fois) ; log `>> OTA: http://<ip>/update`.
  `handleClient()` est appelé à chaque tour de `loop()` dès que le Wi-Fi est connecté ; seul
  `connectTarget()` bloquait plusieurs secondes (maintenant ≤ 5 s). Nouveau `GET /` sans
  authentification : `MCZ bridge OK`, date de build, IP, uptime, état OTA (page de diagnostic si
  seul l'AP de secours est actif).
- **MQTT** (capteurs diagnostic) : `Bridge IP` (`bridge_ip`), `OTA` (`ota_state` : `on` serveur à
  l'écoute / `off` activée mais Wi-Fi absent / `disabled`), `Bridge uptime` (`bridge_uptime`, s),
  `WiFi reconnections` (`wifi_reconnects`, associations après la première).

**Limites / à valider sur le matériel** : le scan BLE à 30 % de rapport cyclique peut retarder la
détection du poêle de quelques secondes ; un Wi-Fi qui ne revient jamais déclenche un reboot
toutes les 10 min ; l'AP de secours (§7) reste ouvert à 60 s sans Wi-Fi, avec la même
coexistence radio qu'avant. Cause non prouvée sur le matériel (hypothèse étayée par le code).
Test : `wifi` en série (doit lister les réseaux), `curl http://<ip>/`.

## 17. Puissance d'émission Wi-Fi réduite et raison du dernier redémarrage

**Symptôme** : alimenté par la sortie d'un autre ESP (et non par l'USB du Mac), le firmware du §16
ne rejoint plus le point d'accès (aucune tentative visible côté hostapd) ; l'ancien firmware, où le
BLE affamait le Wi-Fi, fonctionnait.

**Hypothèse (non prouvée)** : brownout. Depuis le §16 le Wi-Fi émet à pleine puissance (~20 dBm,
pics de 300 à 400 mA) dès le démarrage, ce qu'une alimentation faible ne tient pas.

**Correctifs** (`net_mqtt.cpp`)
- `WiFi.setTxPower(WIFI_TX_POWER)` après chaque `WiFi.mode()` / `WiFi.begin()` (`netBegin()`,
  `wifiBegin()` donc aussi l'association complète du watchdog, passages `WIFI_AP_STA` / `WIFI_STA`
  de l'AP de secours). `WIFI_TX_POWER` : défaut `WIFI_POWER_8_5dBm`, surchargeable dans `config.h`
  (exemple dans `config_example.h`). `setSleep(true)` inchangé ; détecteur de brownout laissé actif.
- Nouveau capteur diagnostic `boot_reason` (`esp_reset_reason()` lu une fois au boot : `poweron`,
  `brownout`, `panic`, `task_wdt`, `int_wdt`, `sw`, `deepsleep`, `ext`, `sdio`, `unknown`), dans le
  payload d'état et la discovery, affiché aussi en série (`>> Boot reason: ...`).
- Le capteur `reset_cause` (registre 0x0351 du POÊLE) est renommé « Poêle : cause de reset »
  (`unique_id` inchangé).

**Limites** : si la liaison devient instable à 8,5 dBm, remonter `WIFI_TX_POWER`. Test : alimenter
par l'autre ESP, vérifier l'association et la valeur de `boot_reason` (`brownout` confirmerait l'hypothèse).

## 18. Recettes « débit de pellets » et « air » (lecture, capteurs bruts, écriture gardée)

**Objet** : exposer les réglages -3..+3 du menu admin de l'appli MCZ. Table M2, groupe `recipes` :
`pos_ric_asp` 0x0578 (1400, air / tirage), `pos_ric_pel` 0x0579 (1401, pellets), `pos_ric_flu` 0x057A
(1402, flux, **lecture seule**) ; registre d'état 867 (0x0363) : `index_ric_flu` (octet **haut**, sub_byte 0)
et `index_ric_pel` (octet **bas**), déjà couvert par le bloc 0x0320 ×77 donc décodé sans nouvelle lecture.
Les 7 positions de chaque recette (1403..1413) ne sont pas lues.

**Codage VALIDE sur le matériel le 21/11** (firmware OTA, build 08:25 UTC) : registre brut = valeur de l'appli + 4
(plage 1..7). Lu au départ : air (0x0578) = 4, pellet (0x0579) = 3, flow (0x057A) = 5 ; l'appli affichait
« pellet -1 » et « Active +1 », et n'a pas de réglage « air ». Après passage de « Active » à 0 dans l'appli :
air = 4, pellet = 3, **flow = 4**. Correspondance : « pellet » de l'appli = 0x0579 (`pos_ric_pel`),
« Active » = 0x057A (`pos_ric_flu`), 0x0578 (`pos_ric_asp`) n'est pas exposé par l'appli : lecture seule ici.
En conséquence l'entité d'écriture « Correction air » (0x0578) est **remplacée** par « Correction active »
(0x057A, `recipe_active_corr`, topic `set/recipe_active_corr`) ; sa discovery est retirée (config vide retenue).
Le texte « HYPOTHESE » ci-dessous date d'avant cette validation. Seule la position brute a été validée :
l'écriture elle-même n'a pas encore été testée.

**Changements** (`oven.h`, `main.cpp`, `net_mqtt.cpp`, `config_example.h`)
- Lecture 0x0578 ×3 insérée dans `pollTick` (un créneau de 2,5 s pris à la place d'un bloc, donc cadence des
  autres blocs inchangée) : après la connexion BLE, puis toutes les 5 min (`RECIPE_POLL_MS`), à la demande
  (`recipes read`) et ~0,8 s après une écriture. Décodée dans la réponse Fn 03 (`recipesApply`), pas dans
  `ovenApplyReg` : l'écho d'une écriture (Fn 06) n'est jamais pris pour une relecture. Réponse `0xFFFF` ou
  lecture en échec : valeur précédente conservée, rien de neuf publié. Valeurs remises à -1 à chaque connexion BLE.
- Capteurs diagnostic MQTT : `recipe_air_raw`, `recipe_pellet_raw`, `recipe_flow_raw`, `recipe_flow_index`,
  `recipe_pellet_index` (clés du JSON d'état = object_id).
- Entités `number` « Correction pellets » (`recipe_pellet_corr`, 0x0579) et « Correction air »
  (`recipe_air_corr`, 0x0578) : min -3, max 3, pas 1, mode box, `entity_category: config`, commande sur
  `mcz/<id>/set/<object_id>`. L'état n'est publié que si le brut relu est dans 1..7 (valeur = brut - 4).
- Écriture (`ovenSetRecipe`, Fn 06 via `sendWrite`, un seul registre) : refusée si brut hors 1..7 ou lecture
  absente / plus vieille de 10 min (log série explicite) ; consigne hors -3..+3 rejetée ; payload MQTT non
  entier rejeté (jamais converti en 0) ; 1 écriture / 10 s / registre ; pas d'écriture si la valeur est déjà
  la bonne. Après écriture : relecture de 0x0578 ×3 et publication de la valeur relue, jamais de la consigne.
- Interrupteur de compilation `RECIPE_WRITE` (défaut 1, `#ifndef` dans `oven.h`, exemple dans
  `config_example.h`) : à 0, pas d'entités `number` (discovery retirée), pas d'abonnement, `ovenSetRecipe` refuse ;
  lecture et capteurs bruts restent.
- Série : `recipes` (brut, index, âge de la lecture), `recipes read` (relecture à la demande).

**Limites** : les registres 1400..1402 sont aussi dans la table `nvm` : chaque écriture est probablement
persistante (usure flash du poêle, d'où la limite de cadence). Effet réel sur la combustion non évalué : tester
de petits écarts, poêle surveillé. Procédure de validation après flash : lancer `recipes`, lire l'appli
(menu admin), comparer brut / appli pour plusieurs positions, et seulement ensuite écrire une valeur.

## 19. Historique persistant des raisons de démarrage

**But** : un ESP alimenté par la sortie d'un autre ESP ne rejoint pas le Wi-Fi, et le rebrancher sur le Mac
efface `esp_reset_reason()`. On veut voir après coup s'il a rebooté en boucle (brownout, panic).

**Changements** (`boothist.cpp/.h`, `main.cpp`, `net_mqtt.cpp`)
- `bootHistRecord()` en tête de `setup()` (avant même `Serial.begin`) : ajoute le code numérique de
  `esp_reset_reason()` à un tampon circulaire de 10 entrées (plus récent d'abord) et incrémente un compteur de
  boots qui ne fait qu'augmenter. NVS, namespace `boothist`, clé `h` : une seule structure de 14 octets, donc
  **une seule écriture NVS par boot**.
- Série : l'historique est affiché au boot ; `boots` l'affiche, `boots clear` le vide (compteur remis à 0).
- MQTT : capteurs diagnostic `boot_history` (chaîne compacte, ex. `sw,brownout,brownout,poweron`) et `boot_count`
  (clés du JSON d'état du même nom). Le capteur `boot_reason` (§17) réutilise `bootReasonName()`.

**Limites** : un boot sans Wi-Fi n'est visible qu'au prochain boot qui rejoint MQTT (ou en série) ; après un
brownout répété l'écriture NVS peut elle-même échouer (message `!! boothist: NVS inaccessible`). Le
dernier code d'un boot en cours de brownout peut manquer si l'ESP retombe avant `setup()`.

**Suite (21/11)**
- Entités : `number` « Correction pellets » (0x0579) et « Correction active » (0x057A) ; `recipe_air_corr`
  retirée ; capteur `recipe_air_raw` renommé « Recette tirage (brut) ».
- Index du registre 867 publiés vides dans HA : le décodage (octet haut = flux, bas = pellets) écartait la valeur
  `0xFF` comme « invalide », donc rien n'était publié si le poêle renvoie 0xFFFF (cause probable, non confirmée
  faute de lecture du registre). Les octets sont maintenant publiés tels quels (0xFF = 255) et un log
  `>> reg 0x0363 (867) = 0x.... -> index flu=.. pel=..` apparaît à chaque changement, ainsi que dans `recipes`.
  Si l'index reste vide, 0x0363 n'est jamais reçu : lancer `r 0363 1`.
- Mode adaptatif : registre 1527 (0x05F7) `mod_ada_on`, type 11 (octet de drapeau, comme `set_orodatario` écrit
  en octet haut), octet haut = `mod_ada_on` (octet bas = `tipo_pul_pot5`). Lecture seule ×1 dans le créneau
  de poll qui suit la lecture des recettes (un créneau de 2,5 s toutes les 5 min en plus) ; `binary_sensor`
  diagnostic « Mode adaptatif » (`adaptive_mode`, ON/OFF ; octet haut hors 0..1 = inconnu, non publié).
  Aucune écriture. Valeur attendue : OFF (affiché « off » dans l'appli).
