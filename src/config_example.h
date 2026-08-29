// config_example.h — Template. Copy this file to src/config.h and enter your
// values. src/config.h is in .gitignore (contains secrets) and is NOT checked in.
//
//   cp src/config_example.h src/config.h
//
// Note: These values are only DEFAULTS / first boot. At runtime, WiFi, MQTT broker
// and target oven are stored in NVS (via the display system menu) and take precedence.
#pragma once

// ---- WiFi ----
#define WIFI_SSID      "SSID"
#define WIFI_PASSWORD  "PASSSWORD"

// ---- MQTT broker ----
#define MQTT_HOST      "192.168.1.10"   // IP or hostname of the broker
#define MQTT_PORT      1883
#define MQTT_USER      ""               // leave empty if no login
#define MQTT_PASSWORD  ""

// ---- Device/topic identity ----
// DEVICE_ID determines topic prefix mcz/<id>/ and HA unique_id/identifiers.
// EMPTY "" = automatically use the oven BLE MAC (recommended: unique per oven, no typing).
// Only set a fixed string if you deliberately want to assign the ID yourself.
#define DEVICE_ID      ""
#define DEVICE_NAME    "MCZ Maestro+ Pelletstove"

// First-boot target oven (important with multiple ovens in range).
// Empty "" = first available oven named MCZ_EP...; otherwise the oven BLE MAC (with or without ':').
// At runtime this is overridden by the NVS value — switch ovens without reflashing via
// serial 'scan' + 'target <mac>' (or 'target none'), or Display -> Find stove.
#define TARGET_MAC     ""

// ---- Oven clock timezone (NTP -> oven RTC via BLE) ----
// The ESP fetches time from NTP (needs internet on the ESP's WiFi) and sets the oven clock.
// POSIX TZ string with automatic daylight-saving. Pick the one for your country:
//   Germany / Italy (CET/CEST): "CET-1CEST,M3.5.0,M10.5.0/3"
//   United Kingdom  (GMT/BST):  "GMT0BST,M3.5.0/1,M10.5.0"
#define OVEN_TZ        "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Home Assistant MQTT discovery ----
// Discovery prefix of Home Assistant (default: "homeassistant").
#define HA_DISCOVERY_PREFIX "homeassistant"

// ---- IP statique (optionnel) -----------------------------------------------
// Supprime l'etape DHCP. Utile quand le point d'acces sert le bail lentement :
// le firmware attend WL_CONNECTED, qui exige une IP.
// Choisir une adresse HORS du pool DHCP du point d'acces.
#define USE_STATIC_IP  0
#define STATIC_IP      "192.168.0.20"
#define STATIC_GW      "192.168.0.1"
#define STATIC_MASK    "255.255.255.0"
#define STATIC_DNS     "192.168.0.1"

// ---- Point d'acces de secours (diagnostic) ---------------------------------
// Si le WiFi n'est pas connecte apres FALLBACK_AP_DELAY_S secondes, l'ESP ouvre
// son propre SSID et sert une page d'etat en texte brut sur http://192.168.4.1/
// (status WiFi, MAC, RSSI, etat MQTT et BLE, scan des reseaux visibles).
// Il continue d'essayer le WiFi normal et referme l'AP des qu'il y arrive.
#define FALLBACK_AP           1
#define FALLBACK_AP_SSID      "MCZ-Bridge"
#define FALLBACK_AP_PASS      "mczbridge"      // 8 caracteres minimum (WPA2)
#define FALLBACK_AP_DELAY_S   60
