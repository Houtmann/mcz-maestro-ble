// diag.h — build de diagnostic (env esp32dev_diag, -DMCZ_DIAG=1). PATCHES.md §12.
// Sans MCZ_DIAG tous les macros sont vides : le build normal ne contient rien de ceci.
// Les evenements sont deposes dans une file bornee (tampons statiques, aucune allocation)
// depuis le callback BLE, et publies sur MQTT depuis loop() (diagNext, appele par netTick).
#pragma once
#include <Arduino.h>

#ifdef MCZ_DIAG
extern const char* g_diagSrc;                       // origine de l'appel en cours a ovenApplyReg : "##", "03", "06"
void   diagCheck(uint16_t reg, uint16_t raw, uint16_t lastBase, uint16_t lastCount); // hors plage attendue -> evenement "rej"
void   diagCaps(bool hydro, uint16_t raw);          // g_caps.hydro a change -> evenement "caps" (prioritaire)
void   diagHash(const uint8_t* img, size_t len, uint16_t base);  // image ## recue -> capture bornee
size_t diagNext(char* out, size_t cap);             // loop : prochain message JSON a publier (0 = rien)

#define DIAG_SRC(s)         (g_diagSrc = (s))
#define DIAG_CHECK(r, v)    diagCheck((r), (v), g_lastReadBase, g_lastReadCount)
#define DIAG_CAPS(h, raw)   do { if (g_caps.hydro != (h)) diagCaps((h), (raw)); } while (0)
#define DIAG_HASH(p, n, b)  diagHash((p), (n), (b))
#else
#define DIAG_SRC(s)         do {} while (0)
#define DIAG_CHECK(r, v)    do {} while (0)
#define DIAG_CAPS(h, raw)   do {} while (0)
#define DIAG_HASH(p, n, b)  do {} while (0)
#endif
