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
