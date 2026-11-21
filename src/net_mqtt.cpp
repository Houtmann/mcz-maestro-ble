// net_mqtt.cpp — WiFi + MQTT + Home Assistant discovery.
// Device identity = oven BLE MAC (unique across multiple ovens); display name incl.
// serial number. Both are only known after the BLE connect -> topics/discovery are
// built lazily once the identity is known.
#include "net_mqtt.h"

#if USE_MQTT
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "appconfig.h"
#include "oven.h"
#include "diag.h"
#include "boothist.h"
// OTA HTTP (PATCHES §14) : compilee si OTA_PASSWORD est defini dans config.h, active si non vide.
// Le serveur web est le meme que celui du point d'acces de secours (un seul WebServer, port 80).
#if defined(OTA_PASSWORD)
#define OTA_COMPILED 1
#endif
#if (defined(FALLBACK_AP) && (FALLBACK_AP == 1)) || defined(OTA_COMPILED)
#include <WebServer.h>   // apres config.h : la macro doit etre connue
#define HAVE_WEBSRV 1
#endif
#ifdef OTA_COMPILED
#include <Update.h>
static const bool OTA_ON = sizeof(OTA_PASSWORD) > 1;   // chaine vide -> desactivee
#endif

#ifdef HAVE_WEBSRV
static WebServer  g_diagSrv(80);
#endif
// Etat de la liaison WiFi (PATCHES §16) : association, watchdog, compteurs exposes en MQTT.
static uint32_t   g_wifiAttemptMs  = 0;      // debut de la tentative d'association en cours
static uint32_t   g_wifiDownSince  = 0;      // debut de la coupure (0 = depuis le demarrage)
static uint32_t   g_wifiReconnects = 0;      // associations apres la premiere
static bool       g_wifiWasUp = false, g_wifiEverUp = false;
static const uint32_t WIFI_BLE_HOLD_MS = 30000;    // BLE muet pendant les 30 premieres s d'une association
static const uint32_t WIFI_FULL_RESTART_MS = 60000;   // sans connexion : disconnect(true)+begin complet
static const uint32_t WIFI_REBOOT_MS = 600000;        // sans connexion depuis 10 min : ESP.restart()
// Puissance d'emission WiFi (PATCHES §17) : reglable dans config.h, 8.5 dBm par defaut
// (pics de courant plus faibles que les ~20 dBm par defaut ; AP a moins de 1 m).
#ifndef WIFI_TX_POWER
#define WIFI_TX_POWER WIFI_POWER_8_5dBm
#endif
static void wifiTxPower(){ WiFi.setTxPower(WIFI_TX_POWER); }
// Raison du dernier redemarrage de l'ESP (PATCHES §17), lue une fois au boot.
static const char* bootReasonStr(){ return bootReasonName((int)esp_reset_reason()); }
static const char* g_bootReason = bootReasonStr();
#if defined(FALLBACK_AP) && (FALLBACK_AP == 1)
static bool       g_apUp = false;
static String     g_scanCache;
#endif
#ifdef OTA_COMPILED
static bool       g_otaUp = false;          // serveur demarre sur l'interface STA
static bool       g_otaAuthed = false;      // le televersement en cours est authentifie
static bool       g_otaFailed = false;
static String     g_otaErr;
#endif
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static String T_BASE, T_STATE, T_AVAIL, T_SET_TEMP, T_SET_POWER, T_SET_MODE, T_SET_ONOFF, T_SET_FAN, T_SET_SILENT, T_SET_ALARM_RESET;
static String g_id, g_devName;
#ifdef MCZ_DIAG
static String T_DEBUG;                 // mcz/<id>/debug/attrs : tous les evenements de diagnostic, sans retain
#endif

static bool     g_idReady     = false;
static String   g_discSerial;                  // serial number that was in the last discovery
static uint32_t g_lastPubSeq  = 0xFFFFFFFF;
static uint32_t g_lastPubMs   = 0;
static bool     g_lastAvailBle= false;
static const uint32_t HEARTBEAT_MS = 30000;

// Hydro-only settings exposed as HA number entities (read/write, °C). One table drives
// discovery, the state JSON and the command handling. object_id = state-JSON key = set/<id> topic.
struct HydroNum { const char* oid; const char* name; uint16_t reg; float mn, mx, step; bool needsPuffer; };
static const HydroNum HYDRO_NUMS[] = {
  {"hyst_amb_neg", "Ambient hysteresis -",        REG_HYST_AMB_NEG, 0, 20, 0.1f, false},
  {"hyst_amb_pos", "Ambient hysteresis +",        REG_HYST_AMB_POS, 0, 20, 0.1f, false},
  {"ss_neg",       "Start/Stop hysteresis -",     REG_HYST_SS_NEG,  0, 20, 0.1f, false},
  {"ss_pos",       "Start/Stop hysteresis +",     REG_HYST_SS_POS,  0, 20, 0.1f, false},
  {"pump_min_on",  "Circulation pump min. temp.", REG_PUMP_MIN_ON, 20, 90, 1.0f, false},
  {"water_set",    "Water temperature setpoint",  REG_SET_PUFFER,  30, 85, 1.0f, true },  // set_puffer
};
static const int HYDRO_NUM_CNT = sizeof(HYDRO_NUMS)/sizeof(HYDRO_NUMS[0]);
static bool hydroNumAvail(const HydroNum& h){ return !h.needsPuffer || g_caps.puffer; }
static float hydroVal(uint16_t reg){    // current OvenState value for a hydro-param register
  switch(reg){
    case REG_HYST_AMB_NEG: return g_oven.hystAmbNeg;
    case REG_HYST_AMB_POS: return g_oven.hystAmbPos;
    case REG_HYST_SS_NEG:  return g_oven.hystSSNeg;
    case REG_HYST_SS_POS:  return g_oven.hystSSPos;
    case REG_PUMP_MIN_ON:  return g_oven.pumpMinOn;
    case REG_SET_PUFFER:   return g_oven.pufferSet;
  } return NAN;
}

// ---- Identity / Topics ----------------------------------------------------
static void buildDevName(){
  // Name uses the same MAC as MQTT topic/ID (g_id) -> immediately correct, no waiting for serial.
  g_devName = String(DEVICE_NAME) + " (" + g_id + ")";
}
static bool buildIdentity(){
  String cfg = DEVICE_ID;                       // optional override from config.h
  String id  = cfg.length() ? cfg : g_ovenMac;  // otherwise: oven BLE MAC
  if (id.length() == 0) return false;           // no identity yet (BLE not connected)
  g_id        = id;
  T_BASE      = String("mcz/") + g_id;
  T_STATE     = T_BASE + "/state";
  T_AVAIL     = T_BASE + "/availability";
#ifdef MCZ_DIAG
  T_DEBUG     = T_BASE + "/debug/attrs";
#endif
  T_SET_TEMP  = T_BASE + "/set/temp";
  T_SET_POWER = T_BASE + "/set/power";
  T_SET_MODE  = T_BASE + "/set/mode";
  T_SET_ONOFF = T_BASE + "/set/onoff";
  T_SET_FAN   = T_BASE + "/set/fan";
  T_SET_SILENT= T_BASE + "/set/silent";
  T_SET_ALARM_RESET = T_BASE + "/set/alarm_reset";
  buildDevName();
  return true;
}

// ---- Helpers --------------------------------------------------------------
static void addDevice(JsonObject o){
  JsonObject dev = o["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add(g_id);
  dev["name"]         = g_devName;
  dev["manufacturer"] = "MCZ";
  // Model = Maestro + detected database id (banca_dati) + AIR/HYDRO once known.
  String model = "Maestro";
  if (g_caps.detected){
    if (g_caps.bancaDati.length()) model += " " + g_caps.bancaDati;
    model += g_caps.hydro ? " (Hydro)" : " (Air)";
  }
  dev["model"]        = model;
  if (g_ovenSerial.length()) dev["serial_number"] = g_ovenSerial;
}
static void publishJson(const String& topic, JsonDocument& doc, bool retain){
  char buf[2048];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retain);
  if (!ok){ mqtt.loop(); delay(10); ok = mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retain); }
  if (!ok) Serial.printf("!! MQTT publish failed (%s, %u B) - buffer/network?\n", topic.c_str(), (unsigned)n);
  // Flush the socket so the discovery burst does not overflow the TCP send buffer
  // (otherwise individual publishes fail -> HA does not create entities).
  mqtt.loop(); delay(2);
}
static String discoTopic(const char* component, const char* objectId){
  return String(HA_DISCOVERY_PREFIX) + "/" + component + "/" + g_id + "_" + objectId + "/config";
}
static const char* otaState(){          // "on" = serveur a l'ecoute, "off" = active mais WiFi absent, "disabled"
#ifdef OTA_COMPILED
  if (!OTA_ON) return "disabled";
  return g_otaUp ? "on" : "off";
#else
  return "disabled";
#endif
}
// Indice FR indicatif par code d'alarme (PATCHES §15) ; nullptr = pas d'attribut. A03 : delai seulement.
static const char* alarmHint(int code){
  switch(code){
    case 2:  return "indicatif : echec allumage / extinction flamme";
    case 4:  return "indicatif : surchauffe fumees";
    case 5:  return "indicatif : defaut debit d'air / depression";
    case 8:  return "indicatif : ventilateur fumees";
    case 9:  return "indicatif : sonde fumees";
    case 12: return "indicatif : vis sans fin";
    case 13: return "indicatif : surchauffe carte electronique";
    case 14: return "indicatif : debit d'air hors plage";
    case 22: return "indicatif : sonde d'ambiance";
    default: return nullptr;
  }
}
// Template « {{ value_json.K if defined else '' }} » pour les capteurs ajoutes (cle absente = pas de valeur).
static String optTpl(const char* key){
  return String("{{ value_json.") + key + " if value_json." + key + " is defined else '' }}";
}
// Supprime une entite HA : config de decouverte vide, retenue, sur l'ancien topic.
static void removeDiscovery(const char* component, const char* objectId){
  String t = discoTopic(component, objectId);
  mqtt.publish(t.c_str(), (const uint8_t*)"", 0, true);
  mqtt.loop(); delay(2);
}
static void addSensor(const char* objectId, const char* name, const char* valTpl,
                      const char* unit, const char* devClass, const char* stateClass,
                      const char* category = nullptr, bool enabled = true){
  JsonDocument d;
  d["name"]               = name;
  d["unique_id"]          = g_id + "_" + objectId;
  d["availability_topic"] = T_AVAIL;
  d["state_topic"]        = T_STATE;
  d["value_template"]     = valTpl;
  if (unit && *unit)             d["unit_of_measurement"] = unit;
  if (devClass && *devClass)     d["device_class"] = devClass;
  if (stateClass && *stateClass) d["state_class"] = stateClass;
  if (category && *category)     d["entity_category"] = category;
  if (!enabled)                  d["enabled_by_default"] = false;
  addDevice(d.as<JsonObject>());
  publishJson(discoTopic("sensor", objectId), d, true);
}

static void publishDiscovery(){
  buildDevName();                 // always build display name from current serial number
  { // climate
    JsonDocument d;
    d["name"]      = "Stove";
    d["unique_id"] = g_id + "_climate";
    d["availability_topic"] = T_AVAIL;
    d["current_temperature_topic"]    = T_STATE;
    d["current_temperature_template"] = "{{ value_json.room }}";
    d["temperature_command_topic"]    = T_SET_TEMP;
    d["temperature_state_topic"]      = T_STATE;
    d["temperature_state_template"]   = "{{ value_json.setpoint }}";
    d["min_temp"] = TEMP_MIN_C; d["max_temp"] = TEMP_MAX_C; d["temp_step"] = 0.5;
    d["temperature_unit"] = "C";
    JsonArray modes = d["modes"].to<JsonArray>(); modes.add("off"); modes.add("heat");
    d["mode_command_topic"]  = T_SET_ONOFF;
    d["mode_state_topic"]    = T_STATE;
    d["mode_state_template"] = "{{ 'heat' if value_json.running else 'off' }}";
    JsonArray pm = d["preset_modes"].to<JsonArray>();
    pm.add("Comfort"); pm.add("Overnight"); pm.add("Turbo"); pm.add("Auto"); pm.add("Manual");
    d["preset_mode_command_topic"]  = T_SET_MODE;
    d["preset_mode_state_topic"]    = T_STATE;
    d["preset_mode_value_template"] = "{{ value_json.mode_name }}";
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("climate", "climate"), d, true);
  }
  { // number: power
    JsonDocument d;
    d["name"]      = "Power";
    d["unique_id"] = g_id + "_power";
    d["availability_topic"] = T_AVAIL;
    d["command_topic"] = T_SET_POWER;
    d["state_topic"]   = T_STATE;
    d["value_template"]= "{{ value_json.power if value_json.power is defined else '' }}";
    d["min"] = 1; d["max"] = 5; d["step"] = 1; d["mode"] = "slider";
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("number", "power"), d, true);
  }
  // Fan/Silent only exist if the stove actually has a controllable fan (capability scan).
  int fanLevels = g_caps.detected ? g_caps.fanLevels : 5;
  bool hasFan   = !g_caps.detected || g_caps.fanCount > 0;   // before detection: assume yes
  if (hasFan){ // select: fan level (Auto + 1..fanLevels, per detected hardware)
    JsonDocument d;
    d["name"]      = "Fan";
    d["unique_id"] = g_id + "_fan";
    d["availability_topic"] = T_AVAIL;
    d["command_topic"] = T_SET_FAN;
    d["state_topic"]   = T_STATE;
    // Live level 0x0324 (1..N) -> option; Auto is not distinguishable from the actual value.
    d["value_template"]= "{{ value_json.fan_level | string if value_json.fan_level is defined else '' }}";
    JsonArray op = d["options"].to<JsonArray>();
    op.add("Auto");
    for (int i=1;i<=fanLevels;i++){ char b[4]; snprintf(b,sizeof(b),"%d",i); op.add(b); }
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("select", "fan"), d, true);
  }
  for (int fi = 2; fi <= 3; fi++){          // ducted fans: set_vent_v2 / set_vent_v3
    if (g_caps.detected && g_caps.fanCount < fi) continue;
    JsonDocument d;
    char nm[8], oid[8];
    snprintf(nm,  sizeof(nm),  "Fan %d", fi);
    snprintf(oid, sizeof(oid), "fan%d",  fi);
    d["name"]      = nm;
    d["unique_id"] = g_id + "_" + oid;
    d["availability_topic"] = T_AVAIL;
    d["command_topic"] = T_BASE + "/set/" + oid;
    d["state_topic"]   = T_STATE;
    d["value_template"]= String("{{ value_json.") + oid + " if value_json." + oid + " is defined else '' }}";
    JsonArray op = d["options"].to<JsonArray>();
    op.add("Auto");
    for (int i=1;i<=fanLevels;i++){ char b[4]; snprintf(b,sizeof(b),"%d",i); op.add(b); }
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("select", oid), d, true);
  }
  if (hasFan){ // switch: silent mode
    JsonDocument d;
    d["name"]      = "Silent";
    d["unique_id"] = g_id + "_silent";
    d["availability_topic"] = T_AVAIL;
    d["command_topic"] = T_SET_SILENT;
    d["state_topic"]   = T_STATE;
    d["value_template"]= "{{ 'ON' if value_json.silent else 'OFF' }}";
    d["payload_on"]  = "on"; d["payload_off"] = "off";
    d["state_on"]    = "ON"; d["state_off"]   = "OFF";
    d["icon"]        = "mdi:volume-off";
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("switch", "silent"), d, true);
  }
  // Hydro-only sensors: published solely when the capability scan found the circuit.
  if (g_caps.hydro)  addSensor("boiler_temp", "Boiler temperature", "{{ value_json.boiler_temp }}", "°C", "temperature", "measurement");
  if (g_caps.puffer) addSensor("puffer_temp", "Buffer temperature", "{{ value_json.puffer_temp }}", "°C", "temperature", "measurement");
  if (g_caps.hydro) for (int i=0;i<HYDRO_NUM_CNT;i++){   // hydro settings as writable number entities
    const HydroNum& h = HYDRO_NUMS[i];
    if (!hydroNumAvail(h)) continue;                     // e.g. water setpoint only if a puffer exists
    JsonDocument d;
    d["name"]      = h.name;
    d["unique_id"] = g_id + "_" + h.oid;
    d["availability_topic"] = T_AVAIL;
    d["command_topic"] = T_BASE + "/set/" + h.oid;
    d["state_topic"]   = T_STATE;
    d["value_template"]= String("{{ value_json.") + h.oid + " if value_json." + h.oid + " is defined else '' }}";
    d["min"] = h.mn; d["max"] = h.mx; d["step"] = h.step;
    d["unit_of_measurement"] = "°C"; d["mode"] = "box"; d["entity_category"] = "config";
    addDevice(d.as<JsonObject>());
    publishJson(discoTopic("number", h.oid), d, true);
  }
  addSensor("fumes", "Flue gas temperature", "{{ value_json.fumes }}",  "°C", "temperature", "measurement");
  addSensor("board", "Control board temperature", "{{ value_json.board }}", "°C", "temperature", "measurement");
  addSensor("room",  "Room temperature",     "{{ value_json.room }}",   "°C", "temperature", "measurement");
  // Registres 718-722 / 712-713 : voir PATCHES §13. `fan_room` garde son unique_id (c'est bien
  // 0x02D1 = consigne ventilateur fumees, seul le libelle etait imprecis). `fan_comb` et `active`
  // lisaient autre chose que ce qu'ils annoncaient : supprimes, remplaces par des entites neuves.
  addSensor("fan_room", "Flue gas fan setpoint", "{{ value_json.fumes_fan_set }}", "rpm", "", "measurement");
  addSensor("fumes_fan_speed", "Flue gas fan speed", "{{ value_json.fumes_fan_speed }}", "rpm", "", "measurement");
  addSensor("auger_set",   "Auger speed setpoint", "{{ value_json.auger_set }}",   "", "", "measurement");
  addSensor("auger_speed", "Auger speed",          "{{ value_json.auger_speed }}", "", "", "measurement");
  addSensor("air_pressure_pa", "Air pressure",     "{{ value_json.air_pressure_pa }}", "Pa", "pressure", "measurement");
  addSensor("air_pressure",    "Air pressure (raw)", "{{ value_json.air_pressure }}", "", "", "measurement");
  addSensor("power_actual", "Power (actual)",      "{{ value_json.power_actual }}", "", "", "measurement");
  removeDiscovery("sensor", "fan_comb");   // ancien « Combustion fan » (0x02CE = consigne vis sans fin)
  removeDiscovery("sensor", "active");     // ancien « Active » (0x02C9 = pression d'air)
  { JsonDocument d;                              // Phase + attributs fase_op / sub_fase_op (octets bas / haut de 0x0320)
    d["name"]="Phase"; d["unique_id"]=g_id+"_phase";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.phase_name }}";
    d["json_attributes_topic"]=T_STATE;
    d["json_attributes_template"]="{% if value_json.fase_op is defined %}{{ {'fase_op': value_json.fase_op, 'sub_fase_op': value_json.sub_fase_op} | tojson }}{% else %}{}{% endif %}";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","phase"), d, true);
  }
  addSensor("ignitions", "Ignitions", "{{ value_json.ignitions }}", "", "", "total_increasing");
  addSensor("worktime", "Total working time", "{{ value_json.worktime_h }}", "h", "duration", "total_increasing");
  for (int i=1;i<=5;i++){
    char oid[12], name[20], tpl[40];
    snprintf(oid,  sizeof(oid),  "ptime%d", i);
    snprintf(name, sizeof(name), "Time power %d", i);
    snprintf(tpl,  sizeof(tpl),  "{{ value_json.pt%d_h }}", i);
    addSensor(oid, name, tpl, "h", "duration", "total_increasing");
  }
  // Etat du pont (PATCHES §16) : IP, OTA, uptime, reconnexions WiFi.
  addSensor("bridge_ip", "Bridge IP", optTpl("ip").c_str(), "", "", "", "diagnostic");
  addSensor("ota_state", "OTA", optTpl("ota").c_str(), "", "", "", "diagnostic");
  addSensor("bridge_uptime", "Bridge uptime", optTpl("uptime_s").c_str(), "s", "duration", "measurement", "diagnostic");
  addSensor("boot_reason", "Boot reason", optTpl("boot_reason").c_str(), "", "", "", "diagnostic");
  addSensor("boot_history", "Boot history", optTpl("boot_history").c_str(), "", "", "", "diagnostic");   // PATCHES §19
  addSensor("boot_count", "Boot count", optTpl("boot_count").c_str(), "", "", "total_increasing", "diagnostic");
  addSensor("wifi_reconnects", "WiFi reconnections", optTpl("wifi_reconnects").c_str(), "", "", "total_increasing", "diagnostic");
  // Capteurs ajoutes d'apres la table M2 (PATCHES §15). Valeurs hors plage / sentinelles non publiees.
  addSensor("outdoor_temp", "Outdoor temperature", optTpl("outdoor_temp").c_str(), "°C", "temperature", "measurement");
  addSensor("panel_temp", "Panel temperature", optTpl("panel_temp").c_str(), "°C", "temperature", "measurement", "diagnostic");
  addSensor("pellet_autonomy", "Pellet autonomy (raw, a valider)", optTpl("pellet_autonomy").c_str(), "", "", "measurement");
  addSensor("sleep_time", "Sleep time (raw, a valider)", optTpl("sleep_time").c_str(), "", "", "", "diagnostic", false);
  addSensor("bat_v1", "Backup battery 1 voltage", optTpl("bat_v1").c_str(), "V", "voltage", "measurement", "diagnostic", false);
  addSensor("bat_v2", "Backup battery 2 voltage", optTpl("bat_v2").c_str(), "V", "voltage", "measurement", "diagnostic", false);
  addSensor("bat_cv1", "Backup battery 1 charge voltage", optTpl("bat_cv1").c_str(), "V", "voltage", "measurement", "diagnostic", false);
  addSensor("bat_cv2", "Backup battery 2 charge voltage", optTpl("bat_cv2").c_str(), "V", "voltage", "measurement", "diagnostic", false);
  addSensor("bat_level_panel", "Backup battery level (panel)", optTpl("bat_level_panel").c_str(), "%", "", "measurement", "diagnostic", false);
  addSensor("bat_level_abs", "Backup battery level (absolute)", optTpl("bat_level_abs").c_str(), "%", "", "measurement", "diagnostic", false);
  addSensor("bat_status", "Backup battery status (flags)", optTpl("bat_status").c_str(), "", "", "", "diagnostic", false);
  addSensor("bat_capacity", "Backup battery capacity (raw)", optTpl("bat_capacity").c_str(), "", "", "", "diagnostic", false);
  // Live alarm (0x0323): is the oven in an alarm RIGHT NOW? + button to acknowledge it.
  { JsonDocument d;
    d["name"]="Alarm"; d["unique_id"]=g_id+"_alarm_active";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ 'ON' if value_json.alarm_active else 'OFF' }}";
    d["device_class"]="problem";
    d["json_attributes_topic"]=T_STATE;
    d["json_attributes_template"]="{% set a = {'code': value_json.alarm} %}{% if value_json.alarm_hint is defined %}{% set a = dict(a, hint=value_json.alarm_hint) %}{% endif %}{{ a | tojson }}";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("binary_sensor","alarm_active"), d, true);
  }
  { JsonDocument d;
    d["name"]="Acknowledge alarm"; d["unique_id"]=g_id+"_alarm_reset";
    d["availability_topic"]=T_AVAIL; d["command_topic"]=T_SET_ALARM_RESET;
    d["payload_press"]="reset"; d["icon"]="mdi:alert-remove-outline";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("button","alarm_reset"), d, true);
  }
  { JsonDocument d;                              // RSSI WiFi du pont (dBm), diagnostic
    d["name"]="WiFi signal"; d["unique_id"]=g_id+"_rssi";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.rssi }}";
    d["unit_of_measurement"]="dBm"; d["device_class"]="signal_strength"; d["state_class"]="measurement";
    d["entity_category"]="diagnostic";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","rssi"), d, true);
  }
  { JsonDocument d;                              // ore_service (846-847) : heures avant entretien
    d["name"]="Hours to service"; d["unique_id"]=g_id+"_service_h";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.service_h }}";
    d["unit_of_measurement"]="h"; d["entity_category"]="diagnostic"; d["icon"]="mdi:wrench-clock";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","service_h"), d, true);
  }
  { JsonDocument d;                              // dia_res_cau (849) : cause de reset
    d["name"]="Poêle : cause de reset"; d["unique_id"]=g_id+"_reset_cause";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.reset_cause }}";
    d["entity_category"]="diagnostic"; d["icon"]="mdi:restart-alert";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","reset_cause"), d, true);
  }
  // Alarm log (all ovens): last alarm + history (raw Axx codes; meanings are model-specific)
  { JsonDocument d;
    d["name"]="Last alarm"; d["unique_id"]=g_id+"_alarm_last";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.alarm_last }}"; d["icon"]="mdi:alert-circle-outline";
    d["json_attributes_topic"]=T_STATE;
    d["json_attributes_template"]="{% if value_json.alarm_last_hint is defined %}{{ {'hint': value_json.alarm_last_hint} | tojson }}{% else %}{}{% endif %}";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","alarm_last"), d, true);
  }
  { JsonDocument d;
    d["name"]="Alarm history"; d["unique_id"]=g_id+"_alarm_hist";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_STATE;
    d["value_template"]="{{ value_json.alarm_hist }}";
    d["json_attributes_topic"]=T_STATE;
    d["json_attributes_template"]="{{ {'codes': value_json.alarm_codes} | tojson }}";
    d["icon"]="mdi:history";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","alarm_hist"), d, true);
  }
#ifdef MCZ_DIAG
  { JsonDocument d;                              // entite unique du build diagnostic (PATCHES §12)
    d["name"]="Diag pont"; d["unique_id"]=g_id+"_diag";
    d["availability_topic"]=T_AVAIL; d["state_topic"]=T_DEBUG;
    d["value_template"]="{{ value_json.n }}";
    d["json_attributes_topic"]=T_DEBUG;
    d["entity_category"]="diagnostic"; d["icon"]="mdi:bug-outline";
    addDevice(d.as<JsonObject>()); publishJson(discoTopic("sensor","diag"), d, true);
  }
#endif
}

static void publishState(){
  JsonDocument d;
  if (!isnan(g_oven.roomC))     d["room"]     = roundf(g_oven.roomC*10)/10.0;
  if (!isnan(g_oven.setpointC)) d["setpoint"] = roundf(g_oven.setpointC*10)/10.0;
  if (!isnan(g_oven.boardC))    d["board"]    = roundf(g_oven.boardC*10)/10.0;
  if (!isnan(g_oven.fumesC))    d["fumes"]    = roundf(g_oven.fumesC*10)/10.0;
  if (g_oven.power>=1 && g_oven.power<=5) d["power"]    = g_oven.power;
  if (g_oven.mode>=0){ d["mode"] = g_oven.mode; d["mode_name"] = modeName(g_oven.mode); }
  if (g_oven.phase>=0){ d["phase"] = g_oven.phase; d["running"] = ovenRunning(); }
  if (g_oven.state>=0){
    d["state"] = g_oven.state;                 // raw fine-phase code (0x0320)
    const char* sn = stateName(g_oven.state);
    if (sn[0]) d["phase_name"] = sn;
    else { char b[24]; snprintf(b,sizeof(b),"Phase %u/%u",(unsigned)(g_oven.state&0xFF),(unsigned)((g_oven.state>>8)&0xFF)); d["phase_name"] = b; }
    d["fase_op"] = g_oven.state & 0xFF;        // octet bas de 0x0320
    d["sub_fase_op"] = (g_oven.state >> 8) & 0xFF;   // octet haut
  }
  if (g_oven.fanLevel>=1 && g_oven.fanLevel<=6) d["fan_level"] = g_oven.fanLevel;
  if (g_oven.fan2Set>=1 && g_oven.fan2Set<=6) d["fan2"] = (g_oven.fan2Set==6) ? String("Auto") : String((int)g_oven.fan2Set);
  if (g_oven.fan3Set>=1 && g_oven.fan3Set<=6) d["fan3"] = (g_oven.fan3Set==6) ? String("Auto") : String((int)g_oven.fan3Set);
  if (g_oven.fumesFanSet>=0)   d["fumes_fan_set"]   = g_oven.fumesFanSet;    // 0x02D1 consigne ventilateur fumees
  if (g_oven.fumesFanReal>=0)  d["fumes_fan_speed"] = g_oven.fumesFanReal;   // 0x02D2 vitesse reelle
  if (g_oven.augerSet>=0)      d["auger_set"]   = g_oven.augerSet;           // 0x02CE consigne vis sans fin
  if (g_oven.augerReal>=0)     d["auger_speed"] = g_oven.augerReal;          // 0x02CF vitesse reelle
  if (!isnan(g_oven.pressurePa)) d["air_pressure_pa"] = roundf(g_oven.pressurePa*10)/10.0;  // 0x02C8, Pa
  if (!isnan(g_oven.pressure))   d["air_pressure"]    = roundf(g_oven.pressure*10)/10.0;    // 0x02C9, unite inconnue
  if (g_oven.powerNow>=0)      d["power_actual"] = g_oven.powerNow;          // 0x0323 octet haut
  if (!isnan(g_oven.outdoorC)) d["outdoor_temp"] = roundf(g_oven.outdoorC*10)/10.0;   // 0x02C3
  if (!isnan(g_oven.panelC))   d["panel_temp"]   = roundf(g_oven.panelC*10)/10.0;     // 0x02C7
  if (g_oven.pelletAuto>=0)    d["pellet_autonomy"] = g_oven.pelletAuto;              // 0x032A brut
  if (g_oven.sleepT>=0)        d["sleep_time"]   = g_oven.sleepT;                     // 0x0389 (##)
  { static const char* BK[4] = {"bat_v1","bat_v2","bat_cv1","bat_cv2"};
    for (int i=0;i<4;i++) if (!isnan(g_oven.batV[i])) d[BK[i]] = roundf(g_oven.batV[i]*10)/10.0; }
  if (g_oven.batPanel>=0)      d["bat_level_panel"] = g_oven.batPanel;
  if (g_oven.batAbs>=0)        d["bat_level_abs"]   = g_oven.batAbs;
  if (g_oven.batStatus>=0)     d["bat_status"]   = g_oven.batStatus;
  if (g_oven.batCap>=0)        d["bat_capacity"] = g_oven.batCap;
  if (g_oven.serviceH>=0)      d["service_h"]    = g_oven.serviceH;          // 0x034E-F, heures
  if (g_oven.resetCause>=0)    d["reset_cause"]  = g_oven.resetCause;        // 0x0351
  if (g_oven.flagsOk){ d["chrono"] = (g_oven.flags>>6)&1; d["silent"] = (g_oven.flags>>5)&1; }
  if (g_oven.ignitions>=0)   d["ignitions"]    = g_oven.ignitions;
  // Compteurs internes en minutes, publies en heures a 2 decimales (resolution 1 min).
  if (g_oven.worktimeMin>=0) d["worktime_h"] = serialized(String(g_oven.worktimeMin/60.0, 2));
  for (int i=0;i<5;i++) if (g_oven.powerTimeMin[i]>=0){
    char k[8]; snprintf(k,sizeof(k),"pt%d_h",i+1); d[k] = serialized(String(g_oven.powerTimeMin[i]/60.0, 2));
  }
  d["ble"] = g_oven.bleOnline;
  d["uptime_s"] = (uint32_t)(millis()/1000); d["ota"] = otaState(); d["boot_reason"] = g_bootReason; d["boot_history"] = bootHistString(); d["boot_count"] = bootHistCount(); d["wifi_reconnects"] = g_wifiReconnects;
  if (WiFi.status() == WL_CONNECTED){ d["ip"] = WiFi.localIP().toString(); }
  if (WiFi.status() == WL_CONNECTED) d["rssi"] = WiFi.RSSI();   // rafraichi par le heartbeat (HEARTBEAT_MS)
  d["seq"] = g_oven.seq;
  if (g_ovenSerial.length()) d["serial"] = g_ovenSerial;
  if (g_oven.alarm >= 0){                        // live alarm: "A12" while active, else "none"
    d["alarm_active"] = g_oven.alarm > 0;
    if (g_oven.alarm > 0){ char a[8]; snprintf(a,sizeof(a),"A%d",g_oven.alarm); d["alarm"] = a;
      const char* h = alarmHint(g_oven.alarm); if (h) d["alarm_hint"] = h; }
    else d["alarm"] = "none";
  }
  if (g_oven.alarmHist[0] >= 0){                 // alarm log (raw Axx codes, newest first)
    char la[8];
    if (g_oven.alarmHist[0] > 0) snprintf(la,sizeof(la),"A%d",g_oven.alarmHist[0]); else strncpy(la,"none",sizeof(la));
    d["alarm_last"] = la;
    { const char* h = alarmHint(g_oven.alarmHist[0]); if (h) d["alarm_last_hint"] = h; }
    String hist; JsonArray codes = d["alarm_codes"].to<JsonArray>();
    for (int k=0;k<10 && g_oven.alarmHist[k]>=0;k++){
      if (g_oven.alarmHist[k] <= 0) continue;
      if (hist.length()) hist += ",";
      hist += "A"; hist += g_oven.alarmHist[k];
      codes.add(g_oven.alarmHist[k]);
    }
    d["alarm_hist"] = hist;
  }
  if (g_caps.detected){                          // detected hardware capabilities
    d["hydro"]      = g_caps.hydro;
    d["fan_count"]  = g_caps.fanCount;
    d["fan_levels"] = g_caps.fanLevels;
    d["boiler"]     = g_caps.boiler;
    d["puffer"]     = g_caps.puffer;
    if (g_caps.bancaDati.length()) d["model"] = g_caps.bancaDati;
    // Hydro live temperatures (only meaningful / published on water-heating stoves)
    if (g_caps.hydro  && !isnan(g_oven.boilerC)) d["boiler_temp"] = roundf(g_oven.boilerC*10)/10.0;
    if (g_caps.puffer && !isnan(g_oven.pufferC)) d["puffer_temp"] = roundf(g_oven.pufferC*10)/10.0;
    if (g_caps.hydro) for (int i=0;i<HYDRO_NUM_CNT;i++){   // hydro settings
      float v = hydroVal(HYDRO_NUMS[i].reg);
      if (!isnan(v) && v>=HYDRO_NUMS[i].mn && v<=HYDRO_NUMS[i].mx) d[HYDRO_NUMS[i].oid] = roundf(v*10)/10.0;
    }
  }
  publishJson(T_STATE, d, true);
}

static void publishAvail(bool online){
  mqtt.publish(T_AVAIL.c_str(), online ? "online" : "offline", true);
}
#ifdef OTA_COMPILED
static void publishAvailOffline(){      // avant un televersement OTA : HA voit le pont hors ligne
  if (mqtt.connected()){ publishAvail(false); mqtt.loop(); }
}
#endif

// ---- Command callback -----------------------------------------------------
static int parseMode(const String& s){
  String t = s; t.trim(); t.toLowerCase();
  if (t=="0" || t=="manuell" || t=="manual")             return 0;
  if (t=="1" || t=="auto")                               return 1;
  if (t=="2" || t=="nacht" || t=="night" || t=="overnight") return 2;
  if (t=="3" || t=="comfort" || t=="komfort")            return 3;
  if (t=="4" || t=="turbo")                              return 4;
  return -1;
}
static void onMqtt(char* topic, byte* payload, unsigned int len){
  String t(topic), msg; msg.reserve(len);
  for (unsigned i=0;i<len;i++) msg += (char)payload[i];
  msg.trim();
  if      (t == T_SET_TEMP)  ovenSetTemp(msg.toFloat());
  else if (t == T_SET_POWER) ovenSetPower(msg.toInt());
  else if (t == T_SET_MODE)  ovenSetMode(parseMode(msg));
  else if (t == T_SET_ONOFF){ String m=msg; m.toLowerCase();
    if (m=="heat"||m=="on"||m=="1")  ovenSetOnOff(true);
    else if (m=="off"||m=="0")       ovenSetOnOff(false);
  }
  else if (t == T_SET_FAN){ String m=msg; m.toLowerCase();
    ovenSetFan(m=="auto" ? 0 : msg.toInt());        // "Auto"/"0" -> Auto, "1".."5" -> level
  }
  else if (t == T_SET_ALARM_RESET) ovenResetAlarm();
  else if (t == T_SET_SILENT){ String m=msg; m.toLowerCase();
    if (m=="on"||m=="1"||m=="true")   ovenSetSilent(true);
    else if (m=="off"||m=="0"||m=="false") ovenSetSilent(false);
  }
  else if (t == T_BASE + "/set/fan2"){ String m=msg; m.toLowerCase(); ovenSetFanN(2, m=="auto"?0:msg.toInt()); }
  else if (t == T_BASE + "/set/fan3"){ String m=msg; m.toLowerCase(); ovenSetFanN(3, m=="auto"?0:msg.toInt()); }
  else for (int i=0;i<HYDRO_NUM_CNT;i++){                  // hydro settings (set/<oid>)
    if (t == T_BASE + "/set/" + HYDRO_NUMS[i].oid){ ovenSetTempParam(HYDRO_NUMS[i].reg, msg.toFloat()); break; }
  }
}

static bool mqttConnect(){
  String cid = String("mcz-") + g_id;
  bool ok;
  if (g_cfg.mqttUser.length() > 0)
    ok = mqtt.connect(cid.c_str(), g_cfg.mqttUser.c_str(), g_cfg.mqttPass.c_str(),
                      T_AVAIL.c_str(), 0, true, "offline");
  else
    ok = mqtt.connect(cid.c_str(), nullptr, nullptr, T_AVAIL.c_str(), 0, true, "offline");
  if (!ok) return false;
  Serial.printf(">> MQTT connected (id=%s).\n", g_id.c_str());
  mqtt.subscribe(T_SET_TEMP.c_str());
  mqtt.subscribe(T_SET_POWER.c_str());
  mqtt.subscribe(T_SET_MODE.c_str());
  mqtt.subscribe(T_SET_ONOFF.c_str());
  mqtt.subscribe(T_SET_FAN.c_str());
  mqtt.subscribe(T_SET_SILENT.c_str());
  mqtt.subscribe((T_BASE + "/set/fan2").c_str());
  mqtt.subscribe((T_BASE + "/set/fan3").c_str());
  mqtt.subscribe(T_SET_ALARM_RESET.c_str());
  for (int i=0;i<HYDRO_NUM_CNT;i++)                 // hydro settings (harmless if not a hydro oven)
    mqtt.subscribe((T_BASE + "/set/" + HYDRO_NUMS[i].oid).c_str());
  publishDiscovery(); g_discSerial = g_ovenSerial;  // name = MAC -> immediately correct
  publishAvail(g_oven.bleOnline); g_lastAvailBle = g_oven.bleOnline;
  publishState();                     // send state IMMEDIATELY (do not wait for the next tick)
  g_lastPubSeq = g_oven.seq; g_lastPubMs = millis();
  return true;
}

#if defined(FALLBACK_AP) && (FALLBACK_AP == 1)
static String diagPage(){
  String p = "MCZ BLE bridge - diagnostic\n\n";
  p += "uptime      : " + String(millis()/1000) + " s\n";
  p += "WiFi status : " + String((int)WiFi.status()) + "  (3 = connecte)\n";
  p += "SSID cible  : " + g_cfg.wifiSsid + "\n";
  p += "MAC         : " + WiFi.macAddress() + "\n";
  p += "IP STA      : " + WiFi.localIP().toString() + "\n";
  p += "RSSI        : " + String((int)WiFi.RSSI()) + " dBm\n";
  p += "MQTT        : " + g_cfg.mqttHost + ":" + String(g_cfg.mqttPort)
     + "  " + (mqtt.connected() ? "connecte" : "NON connecte") + "\n";
  p += "BLE poele   : " + String(g_oven.bleOnline ? "en ligne" : "hors ligne") + "\n";
  p += "Ambiante    : " + String(g_oven.roomC, 1) + " C\n\n";
  p += "Reseaux vus au demarrage de ce point d'acces :\n" + g_scanCache;
  return p;
}
#endif
#ifdef OTA_COMPILED
// GET / : test de joignabilite sans authentification (HTTP 200 + build + etat).
static void otaRoot(){
#if defined(FALLBACK_AP) && (FALLBACK_AP == 1)
  if (WiFi.status() != WL_CONNECTED){ g_diagSrv.send(200, "text/plain; charset=utf-8", diagPage()); return; }
#endif
  String p = "MCZ bridge OK\nbuild " __DATE__ " " __TIME__ "\n";
  p += "ip " + WiFi.localIP().toString() + "\nuptime " + String(millis()/1000) + " s\nota " + otaState() + "\n";
  g_diagSrv.send(200, "text/plain; charset=utf-8", p);
}
// POST /update (multipart, champ `firmware`), Basic Auth admin / OTA_PASSWORD.
// L'ecriture est synchrone dans handleClient() : la boucle (donc le polling BLE) est suspendue
// pendant le televersement. Reboot seulement si Update.end() reussit ; un binaire invalide
// ou tronque est rejete (Update.hasError) et l'ancien firmware reste en place.
static void otaUpload(){
  HTTPUpload& up = g_diagSrv.upload();
  if (up.status == UPLOAD_FILE_START){
    g_otaAuthed = g_diagSrv.authenticate("admin", OTA_PASSWORD);
    g_otaFailed = false; g_otaErr = "";
    if (!g_otaAuthed) return;
    Serial.printf(">> OTA: reception de '%s'\n", up.filename.c_str());
    publishAvailOffline();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)){ g_otaFailed = true; g_otaErr = Update.errorString(); }
  } else if (!g_otaAuthed || g_otaFailed){
    return;
  } else if (up.status == UPLOAD_FILE_WRITE){
    if (Update.write(up.buf, up.currentSize) != up.currentSize){ g_otaFailed = true; g_otaErr = Update.errorString(); }
  } else if (up.status == UPLOAD_FILE_END){
    if (!Update.end(true)){ g_otaFailed = true; g_otaErr = Update.errorString(); }
    else Serial.printf(">> OTA: %u octets recus, image valide\n", (unsigned)up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED){
    Update.abort(); g_otaFailed = true; g_otaErr = "televersement interrompu";
  }
}
static void otaDone(){
  if (!g_otaAuthed){ g_diagSrv.requestAuthentication(); return; }
  if (g_otaFailed || Update.hasError()){
    Serial.printf("!! OTA rejetee : %s (ancien firmware conserve)\n", g_otaErr.c_str());
    g_diagSrv.send(500, "text/plain", String("FAIL: ") + g_otaErr + "\n");
    return;
  }
  g_diagSrv.send(200, "text/plain", "OK, redemarrage\n");
  Serial.println(">> OTA reussie : redemarrage");
  delay(500); ESP.restart();
}
static void otaStart(){                 // appele quand le WiFi STA est connecte
  static bool handlers = false;         // on() ajoute un handler a chaque appel : une seule fois
  if (!handlers){
    g_diagSrv.on("/", HTTP_GET, otaRoot);
    g_diagSrv.on("/update", HTTP_POST, otaDone, otaUpload);
    handlers = true;
  }
  g_diagSrv.stop(); g_diagSrv.begin(); g_otaUp = true;   // (re)lie le port 80 a chaque (re)connexion STA
  Serial.printf(">> OTA: http://%s/update (POST, champ 'firmware', utilisateur admin ; GET / = test)\n",
                WiFi.localIP().toString().c_str());
}
#endif

// Association complete (utilisee au demarrage et par le watchdog). Modem sleep OBLIGATOIRE avec le BLE.
static void wifiBegin(){
#if defined(USE_STATIC_IP) && (USE_STATIC_IP == 1)
  { IPAddress ip, gw, mask, dns;      // IP fixe -> pas de DHCP, association immediate
    if (ip.fromString(STATIC_IP) && gw.fromString(STATIC_GW) &&
        mask.fromString(STATIC_MASK) && dns.fromString(STATIC_DNS)) {
      if (!WiFi.config(ip, gw, mask, dns)) Serial.println("!! WiFi.config (IP statique) a echoue");
      else Serial.printf(">> IP statique %s (gw %s)\n", STATIC_IP, STATIC_GW);
    } else Serial.println("!! IP statique: adresse invalide dans config.h");
  }
#endif
  WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());
  wifiTxPower();
  WiFi.setSleep(true);                // VERIFIE : setSleep(false) fait paniquer la puce (reboot en boucle)
  g_wifiAttemptMs = millis();
}
bool netBleHold(){                    // WiFi prioritaire : BLE muet pendant l'association
  if (WiFi.status() == WL_CONNECTED) return false;
  return millis() - g_wifiAttemptMs < WIFI_BLE_HOLD_MS;
}

// ---- API ------------------------------------------------------------------
void netBegin(){
#ifdef OTA_COMPILED
  if (!OTA_ON) Serial.println(">> OTA desactivee (OTA_PASSWORD vide dans config.h)");
#else
  Serial.println(">> OTA desactivee (OTA_PASSWORD absent de config.h)");
#endif
  Serial.printf(">> Boot reason: %s\n", g_bootReason);
  WiFi.mode(WIFI_STA);
  wifiTxPower();
  WiFi.setSleep(true);                // BLE+WiFi coexistence: modem sleep MUST be on
                                      // VERIFIE : setSleep(false) fait paniquer la puce (reboot en boucle)
  wifiBegin();
  Serial.printf(">> WiFi: connecting to '%s' ...\n", g_cfg.wifiSsid.c_str());
  mqtt.setServer(g_cfg.mqttHost.c_str(), g_cfg.mqttPort);
  mqtt.setBufferSize(2048);          // large HA discovery payloads (climate ~1 kB)
  mqtt.setCallback(onMqtt);
}

void netTick(){
  static uint32_t lastWifi=0, lastMqtt=0;
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED){
    if (g_wifiWasUp){                 // coupure detectee
      g_wifiWasUp = false; g_wifiDownSince = now; g_wifiAttemptMs = now;
      Serial.println("!! WiFi perdu");
    }
    if (now - g_wifiDownSince > WIFI_REBOOT_MS){
      Serial.printf("!! WiFi absent depuis %lu s : redemarrage\n", (unsigned long)((now - g_wifiDownSince)/1000));
      delay(200); ESP.restart();
    }
    if (now - g_wifiAttemptMs > WIFI_FULL_RESTART_MS){   // reconnect() n'aboutit pas : association complete
      Serial.printf("!! WiFi non associe depuis %lu s : disconnect(true) + begin complet\n",
                    (unsigned long)((now - g_wifiAttemptMs)/1000));
      WiFi.disconnect(true); delay(100);
      wifiBegin(); lastWifi = now;
    } else if (now - lastWifi > 20000){ lastWifi = now; WiFi.reconnect(); }   // 5s -> 20s : laisser le temps a l'association
#if defined(FALLBACK_AP) && (FALLBACK_AP == 1)
    if (!g_apUp && now > (uint32_t)FALLBACK_AP_DELAY_S * 1000UL){
      g_scanCache = "";
      int n = WiFi.scanNetworks();
      for (int i=0;i<n;i++){
        g_scanCache += WiFi.SSID(i) + "  " + String(WiFi.RSSI(i)) + " dBm  ch" + String(WiFi.channel(i));
        if (WiFi.SSID(i) == g_cfg.wifiSsid) g_scanCache += "   <== CIBLE";
        g_scanCache += "\n";
      }
      if (n == 0) g_scanCache = "(aucun reseau visible)\n";
      WiFi.scanDelete();
      WiFi.mode(WIFI_AP_STA); wifiTxPower();
      WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS);
      g_diagSrv.onNotFound([](){ g_diagSrv.send(200, "text/plain; charset=utf-8", diagPage()); });
      g_diagSrv.begin();
      g_apUp = true;
      Serial.printf(">> WiFi indisponible : point d'acces de secours '%s' ouvert, http://192.168.4.1/\n",
                    FALLBACK_AP_SSID);
    }
    if (g_apUp) g_diagSrv.handleClient();
#endif
    return;
  }
  if (!g_wifiWasUp){                  // (re)association reussie
    g_wifiWasUp = true; if (g_wifiEverUp) g_wifiReconnects++; g_wifiEverUp = true; g_wifiDownSince = now;
    Serial.printf(">> WiFi connecte : IP %s RSSI %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
#ifdef OTA_COMPILED
    g_otaUp = false;                 // le serveur est relie a chaque (re)connexion (otaStart ci-dessous)
#endif
  }
#if defined(FALLBACK_AP) && (FALLBACK_AP == 1)
  if (g_apUp){                       // WiFi revenu : refermer l'AP de secours
    g_diagSrv.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); wifiTxPower();
    g_apUp = false; Serial.println(">> WiFi retabli : point d'acces de secours ferme");
#ifdef OTA_COMPILED
    g_otaUp = false;                 // stop() a ferme le serveur : OTA redemarree ci-dessous
#endif
  }
#endif
#ifdef OTA_COMPILED
  if (OTA_ON){
    if (!g_otaUp) otaStart();
    g_diagSrv.handleClient();
  }
#endif
  if (!g_idReady){                    // wait for identity (oven MAC)
    if (buildIdentity()){ g_idReady = true; Serial.printf(">> MQTT ID: %s\n", g_id.c_str()); }
    else return;
  }
  if (!mqtt.connected()){
    if (now - lastMqtt > 3000){ lastMqtt = now; mqttConnect(); }
    return;
  }
  mqtt.loop();
  // Disponibilite : republier periodiquement, pas seulement sur changement.
  // Sinon course avec le testament (LWT) : si le broker publie "offline" APRES que
  // l'ESP se soit reconnecte et ait publie "online", HA reste bloque sur offline
  // alors que les etats continuent d'arriver.
  { static uint32_t lastAvailMs = 0;
    if (g_oven.bleOnline != g_lastAvailBle || now - lastAvailMs > HEARTBEAT_MS){
      publishAvail(g_oven.bleOnline); g_lastAvailBle = g_oven.bleOnline; lastAvailMs = now; } }
  // Serial number AND the capability scan only complete after the connect -> re-publish
  // discovery once each becomes known (serial_number metadata; fan entity/level count
  // matched to the detected hardware).
  static bool g_discCaps = false;
  if (g_ovenSerial != g_discSerial || g_caps.detected != g_discCaps){
    publishDiscovery(); g_discSerial = g_ovenSerial; g_discCaps = g_caps.detected;
  }
  // Publish on change, but throttled to >=1s apart (less heap churn / MQTT load over hours);
  // plus a 30s heartbeat so HA stays fresh even without changes.
  bool changed = (g_oven.seq != g_lastPubSeq) && (now - g_lastPubMs >= 1000);
  if (changed || now - g_lastPubMs > HEARTBEAT_MS){
    publishState(); g_lastPubSeq = g_oven.seq; g_lastPubMs = now;
  }
#ifdef MCZ_DIAG
  { static char dbuf[720];                       // 1 message / s au plus ; perdu si le publish echoue (pas de reessai)
    size_t n = diagNext(dbuf, sizeof dbuf);
    if (n) mqtt.publish(T_DEBUG.c_str(), (const uint8_t*)dbuf, n, false); }
#endif
}

bool netWifiUp(){ return WiFi.status()==WL_CONNECTED; }
bool netMqttUp(){ return mqtt.connected(); }
#else
void netBegin(){}
void netTick(){}
bool netWifiUp(){ return false; }
bool netMqttUp(){ return false; }
bool netBleHold(){ return false; }
#endif
