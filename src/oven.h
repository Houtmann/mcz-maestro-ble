// oven.h — central state model + command API for the MCZ oven.
#pragma once
#include <Arduino.h>

// ---- Verified registers ----------------------
// Control (write, function 06):
static const uint16_t REG_SETPOINT = 0x03F7;  // Setpoint temperature, value = °C * 10
static const uint16_t REG_POWER    = 0x03EB;  // Power 1..5
static const uint16_t REG_MODE     = 0x03E9;  // Operating mode, enum 0x03E9:
                                              // 0=Manual,1=Auto,2=Overnight (verified),
                                              // 3=Comfort,4=Turbo (ASSUMED, verify via read test)
static const uint16_t REG_ONOFF    = 0x038A;  // Power TOGGLE: write 1 = toggle on/off
static const uint16_t REG_ALARM_RESET = 0x038B; // com_reset_allarm (907): write 1 = acknowledge alarm
                                              // (same command type 13 as com_on_off; from register DB)
// Date/time block (status_exchange). Byte layout VERIFIED by read-back on the device
// (binary, not BCD; full year; weekday 0=Monday..6=Sunday). Note: hi/lo is the reverse
// of the informal spec -> use exactly this mapping.
static const uint16_t REG_DT_DAYMON  = 0x0384; // 900: hi=month(1-12), lo=day(1-31)
static const uint16_t REG_DT_YEAR    = 0x0385; // 901: full year (e.g. 2026)
static const uint16_t REG_DT_HRMIN   = 0x0386; // 902: hi=minute(0-59), lo=hour(0-23)
static const uint16_t REG_DT_SECWD   = 0x0387; // 903: hi=weekday(0=Mon..6=Sun), lo=second(0-59)
static const uint16_t REG_DT_TRIGGER = 0x0388; // 904: hi=set_orodatario, lo=am_pm -> write 0x0100 to commit
// Status (read):
static const uint16_t REG_ROOM     = 0x02BC;  // Room temp  ÷10
static const uint16_t REG_BOILER_T = 0x02BF;  // temp_caldaia (703): boiler water temp ÷10 (Hydro)
static const uint16_t REG_PUFFER_T = 0x02C0;  // temp_puffer  (704): buffer tank temp ÷10 (Hydro)
static const uint16_t REG_BOARD    = 0x02C1;  // Control board temp ÷10
static const uint16_t REG_FUMES    = 0x02C5;  // Flue gas temp ÷10
static const uint16_t REG_STATE    = 0x0320;  // Fine phase/state (16-bit code, see stateName)
static const uint16_t REG_PHASE    = 0x0322;  // coarse: 1=Off/Standby, 3=On (incl. start sequence)
static const uint16_t REG_MODE_LIVE= 0x032E;  // Operating mode mirror (same enum 0..4)
static const uint16_t REG_ALARM    = 0x0323;  // 803: LOW byte = tipo_allarme (0 = no active alarm),
                                              // HIGH byte = potenza_att (puissance reellement en cours)
static const uint16_t REG_ALARM_IDX= 0x07EA;  // index_allarme (hi) / num_allarmi (lo) — alarm log head
static const uint16_t REG_FLAGS    = 0x0332;  // flags (818-819, 32 bits, mot bas d'abord) : on ne lit que 818.
                                              // Bit field: Bit6=Chrono, Bit5=Silent
static const uint16_t REG_IGNIT    = 0x0334;  // Ignition counter
static const uint16_t REG_WORK_LO  = 0x0340;  // Total working time (minutes, 32-bit LE word)
static const uint16_t REG_WORK_HI  = 0x0341;
static const uint16_t REG_PTIME_LO = 0x0336;  // Time in power 1..5: 5x 32-bit (minutes),
static const uint16_t REG_PTIME_HI = 0x033F;  // low word first, 0x0336..0x033F
// Bloc 0x02BC x51 (700-750), cles de docs/mcz_m2_registers_en.csv (table status) :
static const uint16_t REG_PRESSURE_PA = 0x02C8; // 712 pressione_pascal : pression d'air, Pa, type 3 (÷10, signe)
static const uint16_t REG_PRESSURE    = 0x02C9; // 713 pressione : pression d'air, type 3 (÷10, signe), unite inconnue
static const uint16_t REG_AUGER_SET   = 0x02CE; // 718 vel_imp_coclea : consigne vitesse vis sans fin (uint16)
static const uint16_t REG_AUGER_REAL  = 0x02CF; // 719 vel_real_coclea : vitesse reelle vis sans fin (uint16)
static const uint16_t REG_FUMES_FAN_SET  = 0x02D1; // 721 vel_imp_ventola_fumi : consigne ventilateur fumees (uint16)
static const uint16_t REG_FUMES_FAN_REAL = 0x02D2; // 722 vel_real_ventola_fumi : vitesse reelle ventilateur fumees (uint16)
static const uint16_t REG_TEMP_EXT    = 0x02C3; // 707 temp_sonda_est : temperature exterieure, type 3 (÷10, signe)
static const uint16_t REG_TEMP_PAN    = 0x02C7; // 711 temp_pan : temperature panneau, type 3
static const uint16_t REG_BAT_V1      = 0x02E6; // 742 battery_voltage_1 (type 3, V) ; 743 _2 ; 744/745 battery_charge_voltage_1/2
static const uint16_t REG_BAT_CV2     = 0x02E9; //   (bloc 700-750 ; « BLDC only » d'apres le CSV)
// Bloc 0x0320 x77 (800-876) :
static const uint16_t REG_PELLET_AUTO = 0x032A; // 810 auto_pellet : autonomie pellets (uint16, unite inconnue)
static const uint16_t REG_BAT_LEVEL   = 0x0365; // 869 hi=panel_battery_charge_level, lo=absolute_battery_charge_level (octets)
static const uint16_t REG_BAT_STATUS  = 0x0368; // 872 battery_status (flags)
static const uint16_t REG_BAT_CAP     = 0x0369; // 873 battery_capacity (uint16)
static const uint16_t REG_SLEEP_T     = 0x0389; // 905 tempo_sleep (uint16) : HORS de nos blocs de lecture, recu seulement via l'image ##
static const uint16_t REG_SERVICE_LO  = 0x034E; // 846-847 ore_service : heures avant entretien (uint32, mot bas d'abord)
static const uint16_t REG_SERVICE_HI  = 0x034F;
static const uint16_t REG_RESET_CAUSE = 0x0351; // 849 dia_res_cau : cause de reset (uint16, diagnostic)
static const uint16_t REG_FAN_SET  = 0x03FA;  // Write: 1..5 = fixed level, 6 = auto ventilation
static const uint16_t REG_FAN2_SET = 0x03FB;  // set_vent_v2 : 2nd (ducted) fan, same encoding
static const uint16_t REG_FAN3_SET = 0x03FC;  // set_vent_v3 : 3rd (ducted) fan, same encoding
static const uint16_t REG_FAN_LIVE = 0x0324;  // Live fan level (follows REG_FAN_SET; automatic under Auto)
static const uint16_t REG_SILENT   = 0x03EC;  // Write: 1 = Silent on, 0 = off (mirrors Flags Bit5)
// Hydro settings (read/write, °C ÷10) — verified against the app on the Hydro oven:
static const uint16_t REG_HYST_AMB_NEG = 0x03F3; // ist_neg_amb: ambient hysteresis negative
static const uint16_t REG_HYST_AMB_POS = 0x03F4; // ist_pos_amb: ambient hysteresis positive
static const uint16_t REG_HYST_SS_NEG  = 0x03F5; // ist_eco_neg_amb: Start/Stop (Ecostop) hysteresis neg
static const uint16_t REG_HYST_SS_POS  = 0x03F6; // ist_eco_pos_amb: Start/Stop (Ecostop) hysteresis pos
static const uint16_t REG_PUMP_MIN_ON  = 0x0403; // 1027 temp_min_circ_on: circulation pump min on-temperature

// ---- Capability detection (which hardware the stove actually has) -----------
// Register numbers from the app decoder (libencdec.so) = Modbus addresses.
// Sentinel 0xFFFF = feature absent/not configured (verified on the device).
static const uint16_t REG_SET_BOILER  = 0x0407;  // set_boiler (1031): valid -> boiler circuit
static const uint16_t REG_MAXPOT_IDRO = 0x040F;  // max_pot_idro (1039): >0 & !=0xFFFF -> HYDRO
static const uint16_t REG_SET_PUFFER  = 0x0412;  // set_puffer (1042): valid -> buffer tank
static const uint16_t REG_VVEN1_LO    = 0x05FD;  // v_ven1 v0..v5 (1533..1535): fan-1 speed table
static const uint16_t REG_VVEN1_HI    = 0x05FF;  //   (byte-packed; count of non-zero = fan levels)
static const uint16_t REG_TIPO_APP    = 0x06A4;  // tipo_app (1700): device type id
static const uint16_t REG_ABI_VEN12   = 0x06F4;  // abi_ven1/2_tlc_app (1780): fan-enable bytes
static const uint16_t REG_ABI_VEN34   = 0x06F5;  // abi_ven3/4_tlc_app (1781)
static const uint16_t REG_CLIMA       = 0x0372;  // 882: HIGH byte = tipo_allarme_clima, LOW byte = stato_clima (0 = absent)
static const uint16_t REG_BANCADATI   = 0x07C6;  // rev_banca_dati (1990) + banca_dati name (1991..1998)
// Recettes (menu admin de l'appli, echelle -3..+3) : PATCHES §18. Codage brut = HYPOTHESE (1..7, 4 = 0 ?).
static const uint16_t REG_RECIPE_AIR    = 0x0578; // 1400 pos_ric_asp : position recette air / tirage (uint16)
static const uint16_t REG_RECIPE_PELLET = 0x0579; // 1401 pos_ric_pel : position recette pellets
static const uint16_t REG_RECIPE_FLOW   = 0x057A; // 1402 pos_ric_flu : position recette flux = « Active » de l'appli (validee 21/11)
static const uint16_t REG_MOD_ADA       = 0x05F7; // 1527 mod_ada_on : mode adaptatif, type 11 (octet), octet HAUT (sub_byte 0) ; octet bas = tipo_pul_pot5
static const uint16_t REG_RECIPE_IDX    = 0x0363; // 867 : octet HAUT = index_ric_flu, octet BAS = index_ric_pel (status, bloc 0x0320)
#if defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"     // RECIPE_WRITE doit etre connu avant le defaut ci-dessous (main.cpp inclut oven.h avant config.h)
#  endif
#endif
#ifndef RECIPE_WRITE
#define RECIPE_WRITE 1   // 0 = aucune ecriture des recettes (entites number absentes, ovenSetRecipe refuse)
#endif
static const uint16_t SENTINEL16      = 0xFFFF;

// Detected stove capabilities. Filled once after connect (see capScan in main.cpp);
// all frontends read this to hide functions the hardware does not have.
struct OvenCaps {
  bool     detected  = false;  // capability scan finished
  bool     hydro     = false;  // water-heating stove (max_pot_idro valid)
  bool     boiler    = false;  // boiler (DHW) circuit present
  bool     puffer    = false;  // buffer tank present
  bool     clima     = false;  // conditioner block present
  uint8_t  fanCount  = 1;      // number of enabled room fans (>=1 typical)
  uint8_t  fanLevels = 5;      // selectable fan speed steps (1..5)
  uint16_t tipoApp   = SENTINEL16;
  uint16_t maxPotIdro= SENTINEL16;
  String   bancaDati;          // model database code (e.g. "ET60")
};
extern OvenCaps g_caps;

static const float TEMP_MIN_C = 5.0f, TEMP_MAX_C = 45.0f;  // Plausibility limits for setpoint

// ---- Running oven state ----------------------------------------------------
struct OvenState {
  float    roomC      = NAN;   // Room temperature
  float    boilerC    = NAN;   // Boiler water temperature (Hydro, 0x02BF)
  float    pufferC    = NAN;   // Buffer tank temperature (Hydro, 0x02C0)
  float    hystAmbNeg = NAN;   // Ambient hysteresis - (0x03F3)
  float    hystAmbPos = NAN;   // Ambient hysteresis + (0x03F4)
  float    hystSSNeg  = NAN;   // Start/Stop (Ecostop) hysteresis - (0x03F5)
  float    hystSSPos  = NAN;   // Start/Stop (Ecostop) hysteresis + (0x03F6)
  float    pumpMinOn  = NAN;   // Circulation pump min on-temperature (0x0403)
  float    pufferSet  = NAN;   // Buffer/water setpoint set_puffer (0x0412)
  float    boardC     = NAN;   // Control board temperature
  float    outdoorC   = NAN;   // Outdoor probe (0x02C3), NaN = absente/invalide
  float    panelC     = NAN;   // Panel temperature (0x02C7)
  float    batV[4]    = {NAN,NAN,NAN,NAN};  // battery_voltage_1/2, battery_charge_voltage_1/2 (0x02E6..E9), V
  int32_t  pelletAuto = -1;    // auto_pellet (0x032A), brut
  int16_t  batPanel   = -1;    // panel_battery_charge_level (0x0365 hi), 0..100
  int16_t  batAbs     = -1;    // absolute_battery_charge_level (0x0365 lo), 0..100
  int32_t  batStatus  = -1;    // battery_status (0x0368)
  int32_t  batCap     = -1;    // battery_capacity (0x0369)
  int32_t  sleepT     = -1;    // tempo_sleep (0x0389), via ## seulement
  float    pressurePa = NAN;   // Air pressure, Pa (0x02C8 pressione_pascal)
  float    pressure   = NAN;   // Air pressure, unite inconnue (0x02C9 pressione)
  float    fumesC     = NAN;   // Flue gas temperature
  float    setpointC  = NAN;   // Setpoint temperature
  int8_t   power      = -1;    // Power 1..5
  int8_t   mode       = -1;    // 0=Manual,1=Auto,2=Overnight,3=Comfort,4=Turbo
  int8_t   phase      = -1;    // coarse 0x0322: 1=Off, 3=On
  int32_t  state      = -1;    // Fine phase 0x0320 (code, see stateName)
  uint16_t flags      = 0;     // Bit field 0x0332 (non signe : bit 15 = bit de flag, pas un signe)
  bool     flagsOk    = false; // flags valide (lu au moins une fois)
  int32_t  ignitions  = -1;    // Ignitions
  int32_t  worktimeMin= -1;    // Total working time in minutes
  int32_t  powerTimeMin[5] = {-1,-1,-1,-1,-1};  // Time in power 1..5 (minutes)
  int32_t  augerSet   = -1;    // Auger speed setpoint (0x02CE vel_imp_coclea)
  int32_t  augerReal  = -1;    // Auger actual speed (0x02CF vel_real_coclea)
  int32_t  fumesFanSet  = -1;  // Flue gas fan setpoint (0x02D1 vel_imp_ventola_fumi)
  int32_t  fumesFanReal = -1;  // Flue gas fan actual speed (0x02D2 vel_real_ventola_fumi)
  int8_t   powerNow   = -1;    // potenza_att (0x0323 high byte): puissance en cours 0..5
  int32_t  serviceH   = -1;    // ore_service (0x034E-F): heures avant entretien
  int32_t  recipeAirRaw    = -1;  // pos_ric_asp (0x0578) brut, -1 = pas encore lu / lecture invalide
  int32_t  recipePelletRaw = -1;  // pos_ric_pel (0x0579) brut
  int32_t  recipeFlowRaw   = -1;  // pos_ric_flu (0x057A) brut
  int16_t  recipeFlowIdx   = -1;  // index_ric_flu (0x0363 octet haut), brut 0..255 (255 = 0xFF tel que lu), -1 = jamais recu
  int16_t  recipePelletIdx = -1;  // index_ric_pel (0x0363 octet bas), idem
  int8_t   adaptiveOn      = -1;  // mod_ada_on (0x05F7 octet haut) : 0/1, -1 = inconnu / valeur hors 0..1
  int32_t  resetCause = -1;    // dia_res_cau (0x0351): cause de reset
  int8_t   fanLevel   = -1;    // Live control fan level (0x0324), 1..5
  int8_t   fan2Set    = -1;    // set_vent_v2 (0x03FB): 1..5, 6 = Auto
  int8_t   fan3Set    = -1;    // set_vent_v3 (0x03FC): 1..5, 6 = Auto
  int16_t  alarm      = -1;    // Live alarm code (0x0323 low byte): 0 = no alarm, >0 = active "Axx"
  int16_t  alarmHist[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};  // last 10 alarm codes, newest first (-1=empty, 0=none)
  bool     bleOnline  = false; // BLE link to oven active
  uint32_t lastUpdateMs = 0;   // millis() of the last value update
  uint32_t lastRxMs   = 0;     // millis() of the last valid BLE response (for status light)
  uint32_t seq        = 0;     // increments on EVERY content change (for publish-on-change)
};
extern OvenState g_oven;

// Device identity (for unique MQTT/HA IDs with multiple ovens):
extern String g_ovenMac;     // Oven BLE MAC without ':' (e.g. "0ab0fa7cab43") -> topic/ID base
extern String g_ovenSerial;  // Serial number from register 0x0ADC (ASCII) -> display name
extern String g_adcHex;      // Raw bytes of 0x0ADC as hex (diagnostic, if no ASCII serial)

inline bool ovenRunning(){ return g_oven.phase == 3; }
inline bool ovenSilent(){ return g_oven.flagsOk && ((g_oven.flags >> 5) & 1); }  // Flags Bit5
static const int OVEN_MODE_COUNT = 5;
inline const char* modeName(int m){
  switch(m){ case 0:return "Manual";  case 1:return "Auto";  case 2:return "Overnight";
             case 3:return "Comfort"; case 4:return "Turbo"; default:return "?"; }
}

// Fine phase/operating state from REG_STATE (0x0320) : octet bas = fase_op, octet haut = sub_fase_op.
// Pas de code « refroidissement » identifie : les codes inconnus restent generiques cote MQTT.
inline const char* stateName(int s){
  switch(s){
    case 0x0000: return "Off";
    case 0x0101: return "Cleaning";
    case 0x0201: return "Loading";
    case 0x0301: return "Start 1";
    case 0x0401: return "Start 2";
    case 0x0501: return "Stabilization";
    case 0x0601: return "Anti-condensation";
    case 0x0202: return "On";
    case 0x0103: return "Turning off";       // sub 1 / phase 3
    case 0x0203: return "Turning off 2";     // sub 2 / phase 3 (hypothese : 2e etape de l'arret)
    default:     return "";
  }
}

// Sort one register (address+value) into g_oven; bumps seq on change.
// Source irrelevant (poll read or ##-broadcast). Implemented in main.cpp.
void ovenApplyReg(uint16_t reg, uint16_t val);

// ---- Command API (used by Serial/Display/MQTT) -----------------------------
// Return true if the write command was issued (value plausible + BLE ready).
bool ovenSetTemp(float celsius);
bool ovenSetPower(int level);    // 1..5
bool ovenSetMode(int mode);      // 0..4 (see REG_MODE enum)
bool ovenSetOnOff(bool on);      // TODO: write register still unknown 
bool ovenSetFan(int level);      // 0 = Auto (writes 6), 1..5 = fixed level -> REG_FAN_SET
bool ovenSetFanN(int idx, int level);  // idx 2|3 -> REG_FAN2_SET / REG_FAN3_SET, same encoding
bool ovenSetSilent(bool on);     // Silent mode on/off -> REG_SILENT
bool ovenSetTempParam(uint16_t reg, float celsius);  // write a °C-scaled setting (value = C*10)
bool ovenSetRecipe(int which, int corr);  // which 1 = pellets (0x0579), 2 = active (0x057A) ; 0 (air) refuse, lecture seule ; corr -3..+3 (brut = corr+4, a valider) ; PATCHES §18
void ovenReadRecipes();          // demande une relecture de 0x0578 x3 (prise en charge par pollTick)
bool ovenSetClock();             // set the oven RTC from NTP (writes 900-903 + trigger 904)
bool ovenResetAlarm();           // acknowledge the active alarm -> REG_ALARM_RESET=1 (only if one is active)
