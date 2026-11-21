// boothist.cpp — voir boothist.h. Une seule structure de 14 octets en NVS = une seule ecriture par boot.
#include "boothist.h"
#include <Preferences.h>
#include <esp_system.h>

static const int BH_N = 10;
struct BootHist { uint32_t count; uint8_t n; uint8_t codes[BH_N]; };   // codes[0] = plus recent
static BootHist g_bh = {0, 0, {0}};
static bool g_bhLoaded = false;

const char* bootReasonName(int code){
  switch (code){
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}
static void bhLoad(){
  g_bh = BootHist{0, 0, {0}};
  Preferences p;
  if (p.begin("boothist", true)){
    if (p.getBytesLength("h") == sizeof(BootHist)) p.getBytes("h", &g_bh, sizeof(BootHist));
    p.end();
  }
  if (g_bh.n > BH_N) g_bh.n = BH_N;       // donnees corrompues : on borne
  g_bhLoaded = true;
}
static void bhSave(){
  Preferences p;
  if (p.begin("boothist", false)){ p.putBytes("h", &g_bh, sizeof(BootHist)); p.end(); }
  else Serial.println("!! boothist: NVS inaccessible");
}
void bootHistRecord(){
  bhLoad();
  memmove(&g_bh.codes[1], &g_bh.codes[0], BH_N-1);
  g_bh.codes[0] = (uint8_t)esp_reset_reason();
  if (g_bh.n < BH_N) g_bh.n++;
  g_bh.count++;
  bhSave();                                // seule ecriture NVS du boot
}
uint32_t bootHistCount(){ if(!g_bhLoaded) bhLoad(); return g_bh.count; }
String bootHistString(){
  if(!g_bhLoaded) bhLoad();
  String s;
  for (int i=0;i<g_bh.n;i++){ if (i) s += ","; s += bootReasonName(g_bh.codes[i]); }
  return s;
}
void bootHistPrint(){
  Serial.printf(">> Boots: %lu au total ; historique (recents d'abord): %s\n",
                (unsigned long)bootHistCount(), bootHistString().length() ? bootHistString().c_str() : "(vide)");
}
void bootHistClear(){
  g_bh = BootHist{0, 0, {0}}; g_bhLoaded = true; bhSave();
  Serial.println(">> Historique de boot vide (le compteur repart de 0)");
}
