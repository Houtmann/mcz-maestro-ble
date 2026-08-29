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
