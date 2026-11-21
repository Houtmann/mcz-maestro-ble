// diag.cpp — build de diagnostic (PATCHES.md §12). Entierement sous MCZ_DIAG.
#include "diag.h"

#ifdef MCZ_DIAG
#include "oven.h"

const char* g_diagSrc = "?";

// ---- File d'evenements (producteur : tache BLE ; consommateur : loop) ------------
static const int   QN   = 8;
static const int   QLEN = 200;
static char        g_q[QN][QLEN];            // corps JSON sans accolades ni "n"
static uint8_t     g_qHead = 0, g_qCnt = 0;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t g_n = 0;                     // compteur d'evenements publies (etat de l'entite)
static uint32_t g_rej = 0, g_drop = 0, g_rlDrop = 0;   // rejets vus / pertes file pleine / pertes limite 30 s
static uint32_t g_rx = 0, g_hashes = 0;      // images ## recues / images retenues

static void push(const char* body, bool force){
  portENTER_CRITICAL(&g_mux);
  if (g_qCnt >= QN){
    g_drop++;
    if (!force){ portEXIT_CRITICAL(&g_mux); return; }
    g_qHead = (g_qHead + 1) % QN; g_qCnt--;  // evenement prioritaire : on ecrase le plus ancien
  }
  strlcpy(g_q[(g_qHead + g_qCnt) % QN], body, QLEN); g_qCnt++;
  portEXIT_CRITICAL(&g_mux);
}

// ---- Evenement "rej" : valeur hors plage attendue (le comportement n'est pas modifie) -----
struct Seen { uint16_t reg, raw; };
static Seen     g_seen[16]; static uint8_t g_seenCnt = 0, g_seenNext = 0;   // dedoublonnage reg+valeur
static uint32_t g_lastRejMs = 0; static bool g_haveRej = false;

static bool outOfRange(uint16_t reg, uint16_t raw){
  switch (reg){
    case REG_HYST_AMB_NEG: case REG_HYST_AMB_POS: case REG_HYST_SS_NEG: case REG_HYST_SS_POS:
      return (int16_t)raw/10.0f < 0.0f || (int16_t)raw/10.0f > 20.0f;   // plage HYDRO_NUMS 0..20 (type 3 signe, comme le decodage)
    case REG_PUMP_MIN_ON:  return (int16_t)raw/10.0f < 20.0f || (int16_t)raw/10.0f > 90.0f;
    case REG_SET_PUFFER:   return raw != SENTINEL16 && ((int16_t)raw/10.0f < 30.0f || (int16_t)raw/10.0f > 85.0f);
    case REG_POWER:        return raw < 1 || raw > 5;
    case REG_FAN_LIVE: case REG_FAN2_SET: case REG_FAN3_SET: return raw > 6;   // 0 = legitime (Auto/arret)
  }
  return false;
}
void diagCheck(uint16_t reg, uint16_t raw, uint16_t lastBase, uint16_t lastCount){
  if (!outOfRange(reg, raw)) return;
  g_rej++;
  for (uint8_t i=0;i<g_seenCnt;i++) if (g_seen[i].reg==reg && g_seen[i].raw==raw) return;
  uint32_t now = millis();
  if (g_haveRej && now - g_lastRejMs < 30000){ g_rlDrop++; return; }
  g_seen[g_seenNext] = {reg, raw}; g_seenNext = (g_seenNext+1) % 16; if (g_seenCnt<16) g_seenCnt++;
  g_lastRejMs = now; g_haveRej = true;
  char b[QLEN];
  snprintf(b, sizeof b, "\"t\":\"rej\",\"reg\":\"0x%04X\",\"raw\":%u,\"src\":\"%s\",\"up\":%lu,"
           "\"lastRead\":\"0x%04X\",\"lastCount\":%u,\"hydro\":%d",
           (unsigned)reg, (unsigned)raw, g_diagSrc, (unsigned long)(now/1000),
           (unsigned)lastBase, (unsigned)lastCount, g_caps.hydro ? 1 : 0);
  push(b, false);
}

void diagCaps(bool hydro, uint16_t raw){
  char b[QLEN];
  snprintf(b, sizeof b, "\"t\":\"caps\",\"hydro\":%d,\"src\":\"%s\",\"raw\":%u,\"up\":%lu",
           hydro ? 1 : 0, g_diagSrc, (unsigned)raw, (unsigned long)(millis()/1000));
  push(b, true);
}

// ---- Capture de l'image ## : 1 image / 10 min, 20 max, la premiere toujours -------------
static const size_t IMG_MAX = 768;                  // = sizeof(g_status) de main.cpp
static uint8_t  g_img[IMG_MAX];
static size_t   g_imgLen = 0; static uint16_t g_imgBase = 0;
static uint32_t g_imgSeq = 0; static uint8_t g_imgPart = 0;
static volatile bool g_imgPending = false;          // vrai de la copie jusqu'a la derniere partie publiee
static uint32_t g_lastHashMs = 0;

void diagHash(const uint8_t* img, size_t len, uint16_t base){
  g_rx++;
  if (g_imgPending || g_hashes >= 20) return;
  uint32_t now = millis();
  if (g_hashes > 0 && now - g_lastHashMs < 600000) return;
  if (len > IMG_MAX) len = IMG_MAX;
  memcpy(g_img, img, len);
  g_imgLen = len; g_imgBase = base; g_imgSeq = ++g_hashes; g_imgPart = 0; g_lastHashMs = now;
  g_imgPending = true;                              // en dernier : le consommateur ne lit qu'apres
}

// ---- Cote loop : un message au plus par seconde -------------------------------------
size_t diagNext(char* out, size_t cap){
  static uint32_t lastOut = 0; static bool any = false;
  static uint32_t lastHb = 0; static bool hbDone = false;
  uint32_t now = millis();
  if (any && now - lastOut < 1000) return 0;

  char b[QLEN]; bool have = false;
  portENTER_CRITICAL(&g_mux);
  if (g_qCnt){ strlcpy(b, g_q[g_qHead], QLEN); g_qHead = (g_qHead+1) % QN; g_qCnt--; have = true; }
  portEXIT_CRITICAL(&g_mux);
  int n = 0;
  if (have){
    n = snprintf(out, cap, "{%s,\"n\":%lu}", b, (unsigned long)++g_n);
  } else if (g_imgPending){
    const size_t CH = 256;
    uint8_t parts = (uint8_t)((g_imgLen + CH - 1) / CH);
    size_t off = (size_t)g_imgPart * CH, chunk = g_imgLen - off; if (chunk > CH) chunk = CH;
    n = snprintf(out, cap, "{\"t\":\"hash\",\"seq\":%lu,\"part\":%u,\"parts\":%u,\"len\":%u,\"off\":%u,"
                 "\"base\":\"0x%04X\",\"hex\":\"", (unsigned long)g_imgSeq, (unsigned)g_imgPart,
                 (unsigned)parts, (unsigned)g_imgLen, (unsigned)off, (unsigned)g_imgBase);
    if ((size_t)n + 2*chunk + 32 >= cap) return 0;
    static const char H[] = "0123456789ABCDEF";
    for (size_t i=0;i<chunk;i++){ out[n++] = H[g_img[off+i]>>4]; out[n++] = H[g_img[off+i]&15]; }
    n += snprintf(out+n, cap-n, "\",\"n\":%lu}", (unsigned long)++g_n);
    if (++g_imgPart >= parts) g_imgPending = false;
  } else if (!hbDone || now - lastHb >= 600000){
    hbDone = true; lastHb = now;
    n = snprintf(out, cap, "{\"t\":\"hb\",\"up\":%lu,\"hashes\":%lu,\"rx\":%lu,\"rej\":%lu,\"drop\":%lu,\"rl\":%lu,"
                 "\"hydro\":%d,\"n\":%lu}", (unsigned long)(now/1000), (unsigned long)g_hashes, (unsigned long)g_rx,
                 (unsigned long)g_rej, (unsigned long)g_drop, (unsigned long)g_rlDrop,
                 g_caps.hydro ? 1 : 0, (unsigned long)++g_n);
  } else return 0;
  lastOut = now; any = true;
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
#endif  // MCZ_DIAG
