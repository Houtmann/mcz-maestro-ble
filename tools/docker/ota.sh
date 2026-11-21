#!/usr/bin/env bash
# Usage : ota.sh <ip> [env]   (env par defaut : esp32dev)
# Compile via build.sh (Docker) puis envoie .pio/build/<env>/firmware.bin en HTTP (curl du Mac).
# Mot de passe : variable d'environnement OTA_PASSWORD, sinon demande (jamais ecrit ni affiche).
set -euo pipefail
IP="${1:?usage: ota.sh <ip> [env]}"
ENV_NAME="${2:-esp32dev}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"   # racine du depot
"$HERE/build.sh" "$ENV_NAME"
if [ -z "${OTA_PASSWORD:-}" ]; then read -rs -p "Mot de passe OTA : " OTA_PASSWORD; echo; fi
# -K - : identifiants lus sur stdin (config curl), pas dans la liste des processus
printf 'user = "admin:%s"\n' "$OTA_PASSWORD" | \
  curl -fsS -K - -F firmware=@"$REPO/.pio/build/$ENV_NAME/firmware.bin" "http://$IP/update"
