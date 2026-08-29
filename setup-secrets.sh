#!/bin/bash
# Renseigne les 4 secrets dans src/config.h — saisie masquée, rien en historique.
cd "$(dirname "$0")" || exit 1
[ -f src/config.h ] || { echo "src/config.h introuvable"; exit 1; }

read -r  -p "SSID WiFi              : " SSID
read -rs -p "Mot de passe WiFi      : " WPASS; echo
read -r  -p "Utilisateur MQTT       : " MUSER
read -rs -p "Mot de passe MQTT      : " MPASS; echo

SSID="$SSID" WPASS="$WPASS" MUSER="$MUSER" MPASS="$MPASS" python3 - <<'PY'
import os, re, json
p = "src/config.h"
s = open(p, encoding="utf-8").read()
for macro, env in (("WIFI_SSID","SSID"), ("WIFI_PASSWORD","WPASS"),
                   ("MQTT_USER","MUSER"), ("MQTT_PASSWORD","MPASS")):
    val = json.dumps(os.environ[env])          # échappe guillemets et antislashs
    s = re.sub(rf'(#define\s+{macro}\s+)"[^"]*"', lambda m: m.group(1)+val, s, count=1)
open(p, "w", encoding="utf-8").write(s)
print("✓ src/config.h mis à jour")
PY

grep -E '#define (WIFI_SSID|MQTT_USER)' src/config.h
echo "(les deux mots de passe sont écrits mais non affichés)"
