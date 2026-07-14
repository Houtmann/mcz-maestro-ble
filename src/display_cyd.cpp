// display_cyd.cpp - Cheap Yellow Display (ESP32-2432S028R): TFT (ILI9341) + LVGL + XPT2046 touch.
// Only active with -DUSE_DISPLAY=1 (env:cyd). Phase 2: read-only status display.
#include "display.h"

#if defined(USE_DISPLAY) && (USE_DISPLAY == 1)

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include "oven.h"
#include "appconfig.h"
#include "ble_api.h"
#include "fan_icon.h"    // generated fan icon (A8, recolorable)

// ---- Touch pins (own SPI bus, separate from the TFT) ----
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// Raw value range of the resistive touch -> comes from g_cfg (calibrated per device, NVS).

static const int16_t SCR_W = 320, SCR_H = 240;   // landscape (rotation 1)

// ---- Multilingual: EN / DE / IT (ASCII-only because of font glyphs) --------
enum { T_ROOM,T_SET,T_POWER,T_ON,T_OFF,T_RUN,T_STOP,T_PHASE,T_MANUAL,T_AUTO,T_NIGHT,T_COMFORT,T_TURBO,
  T_FAN,T_SILENT,
  T_SYSTEM,T_FIND,T_MQTT,T_INFO,T_CALIB,T_SETTINGS,T_SAVEREB,T_BACK,
  T_SCAN,T_RESCAN,T_SEARCHING,T_NOTHING,T_NONETS,T_SAVE,T_WIFIPASS,
  T_HOST,T_PORT,T_USER,T_PASS,T_CALTITLE,T_LANGUAGE,T_DISPTO,T_OFFOPT,T_MAC,T_SERIAL,T_COUNT };
static const char* const STR[T_COUNT][3] = {
  {"Room","Raum","Ambiente"}, {"Set","Soll","Imposta"}, {"Power","Leistung","Potenza"},
  {"ON","EIN","ON"}, {"OFF","AUS","OFF"}, {"Running","Betrieb","In funzione"},
  {"Off","Aus","Spento"}, {"Phase","Phase","Fase"},
  {"Manual","Manuell","Manuale"}, {"Auto","Auto","Auto"}, {"Overnight","Nacht","Notte"},
  {"Comfort","Komfort","Comfort"}, {"Turbo","Turbo","Turbo"},
  {"Fan","Geblaese","Ventola"}, {"Silent","Silent","Silenzioso"},
  {"System","System","Sistema"}, {"Find stove","Ofen suchen","Cerca stufa"},
  {"MQTT broker","MQTT-Broker","Broker MQTT"}, {"Info","Info","Info"},
  {"Calibrate touch","Touch kalibrieren","Calibra touch"},
  {"Display & Language","Anzeige & Sprache","Display e lingua"},
  {"Save & Restart","Speichern & Neustart","Salva e riavvia"}, {"Back","Zurueck","Indietro"},
  {"Scan","Scan","Scansione"}, {"Rescan","Neu scan","Riscansiona"},
  {"Searching...","Suche...","Ricerca..."}, {"nothing found","nichts gefunden","niente trovato"},
  {"no networks","keine Netze","nessuna rete"}, {"Save","Speichern","Salva"},
  {"WiFi password","WiFi-Passwort","Password WiFi"},
  {"Broker host/IP","Broker-Host/IP","Host/IP broker"}, {"Port","Port","Porta"},
  {"User (optional)","User (optional)","Utente (opz.)"},
  {"Password (optional)","Passwort (optional)","Password (opz.)"},
  {"Touch calibration\nTap exactly on the +","Touch-Kalibrierung\nTippe genau auf das +","Calibrazione touch\nTocca la +"},
  {"Language","Sprache","Lingua"}, {"Display timeout","Display-Timeout","Timeout display"},
  {"Off","Aus","Spento"}, {"Stove MAC","Ofen-MAC","MAC stufa"}, {"Serial","Serial","Seriale"},
};
static const char* tr(int id){ uint8_t l=g_cfg.lang; if(l>2) l=0; return STR[id][l]; }

// Screensaver (backlight timeout)
static uint32_t g_lastTouch = 0;
static bool     g_screenOff = false;
static inline void screenWake(){ digitalWrite(TFT_BL, HIGH); g_screenOff=false; g_lastTouch=millis(); }

static TFT_eSPI            tft;
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
// Touch uses the global SPI (VSPI), remapped to the touch pins. The TFT runs on
// the HSPI default pins (12/13/14/15) -> no bus conflict.

// Keep the LVGL draw buffer small (static .bss -> more free heap for BLE+WiFi)
static const uint16_t      LV_BUF_LINES = 10;
static lv_disp_draw_buf_t  draw_buf;
static lv_color_t          lvbuf[SCR_W * LV_BUF_LINES];

// UI elements (updated in render())
static lv_obj_t *lblRoom, *lblSet, *bmPower, *bmMode, *btnOnOff, *lblOnOff, *lblPhase, *lblConn;

static int16_t g_tx = 0, g_ty = 0;   // last touch point

// ---- LVGL callbacks -------------------------------------------------------
static void flush_cb(lv_disp_drv_t *d, const lv_area_t *area, lv_color_t *color_p){
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)&color_p->full, w * h, true);   // true = byteswap
  tft.endWrite();
  lv_disp_flush_ready(d);
}

static bool g_touchCalib = false;  // true: print raw touch values over serial (calibration)
static void touch_cb(lv_indev_drv_t *d, lv_indev_data_t *data){
  static bool ignoreUntilRelease = false;
  if (ts.touched()){
    if (g_screenOff){   // screensaver on: first touch ONLY wakes -> not passed to LVGL
      screenWake(); ignoreUntilRelease = true;
      data->state=LV_INDEV_STATE_REL; data->point.x=g_tx; data->point.y=g_ty; return;
    }
    if (ignoreUntilRelease){   // still the same wake touch (finger down) -> ignore until released
      data->state=LV_INDEV_STATE_REL; data->point.x=g_tx; data->point.y=g_ty; return;
    }
    g_lastTouch = millis();
    TS_Point p = ts.getPoint();
    if (g_touchCalib){
      static uint32_t last=0;
      if (millis()-last > 200){ last=millis(); Serial.printf("[touch] raw x=%d y=%d z=%d\n", p.x, p.y, p.z); }
    }
    int16_t x = map(p.x, g_cfg.tsMinX, g_cfg.tsMaxX, 0, SCR_W);
    int16_t y = map(p.y, g_cfg.tsMinY, g_cfg.tsMaxY, 0, SCR_H);
    x = constrain(x, 0, SCR_W-1); y = constrain(y, 0, SCR_H-1);
    g_tx = x; g_ty = y;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x; data->point.y = y;
  } else {
    ignoreUntilRelease = false;   // finger released -> next touch is a command again
    data->state = LV_INDEV_STATE_REL;
    data->point.x = g_tx; data->point.y = g_ty;
  }
}

// ---- UI: control surface (Phase 3) -------------------------------------
static const char* mapPower[] = {"1","2","3","4","5",""};
// 5 modes in 2 rows (Comfort/Overnight/Turbo | Auto/Manual); filled per language in buildUI.
static const char* mapMode[7] = {"Comfort","Overnight","Turbo","\n","Auto","Manual",""};
// Button index (without "\n") -> register value 0x03E9. 0=Manual,1=Auto,2=Overnight verified;
// 3=Comfort,4=Turbo ASSUMED -> verify on the device via read test.
static const uint8_t MODE_UI2REG[5] = {3, 2, 4, 1, 0};

// Event callbacks -> central command API (oven.h)
static void evSetMinus(lv_event_t*){ float s=isnan(g_oven.setpointC)?20.0f:g_oven.setpointC; ovenSetTemp(s-0.5f); }
static void evSetPlus (lv_event_t*){ float s=isnan(g_oven.setpointC)?20.0f:g_oven.setpointC; ovenSetTemp(s+0.5f); }
static void evPower(lv_event_t* e){ uint16_t id=lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
                                    if(id!=LV_BTNMATRIX_BTN_NONE) ovenSetPower(id+1); }
static void evMode (lv_event_t* e){ uint16_t id=lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
                                    if(id!=LV_BTNMATRIX_BTN_NONE && id<5) ovenSetMode(MODE_UI2REG[id]); }
static void evOnOff(lv_event_t*){ ovenSetOnOff(!ovenRunning()); }
// Fan: button 0=Auto, 1..5=level -> ovenSetFan(0=Auto,1..5)
static const char* mapFan[8] = {"Auto","1","2","3","4","5",""};   // language filled in buildFan
static void evFan(lv_event_t* e){ uint16_t id=lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
                                  if(id!=LV_BTNMATRIX_BTN_NONE && id<=5) ovenSetFan(id); }
static void evSilent(lv_event_t*){ ovenSetSilent(!ovenSilent()); }

static lv_obj_t* mkBtn(lv_obj_t* par, const char* txt, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_event_cb_t cb){
  lv_obj_t* b=lv_btn_create(par); lv_obj_set_pos(b,x,y); lv_obj_set_size(b,w,h);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,txt); lv_obj_center(l);
  return b;
}

static void buildUI(){
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);

  // Bluetooth status symbol top LEFT (color = connection indicator, in render())
  lblConn = lv_label_create(scr); lv_label_set_text(lblConn, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_font(lblConn, &lv_font_montserrat_28, 0);
  lv_obj_align(lblConn, LV_ALIGN_TOP_LEFT, 6, 2);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "MCZ Maestro+");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF0C000), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 4);        // right next to the BT symbol

  // Room temperature (large)
  lv_obj_t* capR=lv_label_create(scr); lv_label_set_text(capR,tr(T_ROOM));
  lv_obj_set_style_text_color(capR,lv_color_hex(0x9aa0a6),0); lv_obj_set_pos(capR,8,38);
  lblRoom=lv_label_create(scr); lv_label_set_text(lblRoom,"--.-");
  lv_obj_set_style_text_font(lblRoom,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(lblRoom,lv_color_hex(0xffffff),0); lv_obj_set_pos(lblRoom,8,54);

  // Setpoint with -/+
  lv_obj_t* capS=lv_label_create(scr); lv_label_set_text(capS,tr(T_SET));
  lv_obj_set_style_text_color(capS,lv_color_hex(0x9aa0a6),0); lv_obj_set_pos(capS,176,38);
  mkBtn(scr, LV_SYMBOL_MINUS, 168, 56, 40, 40, evSetMinus);
  lblSet=lv_label_create(scr); lv_label_set_text(lblSet,"--.-");
  lv_obj_set_style_text_font(lblSet,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(lblSet,lv_color_hex(0xffffff),0);
  lv_obj_set_size(lblSet,52,20); lv_obj_set_style_text_align(lblSet,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_pos(lblSet,212,66);
  mkBtn(scr, LV_SYMBOL_PLUS, 272, 56, 40, 40, evSetPlus);

  // Power 1-5
  lv_obj_t* capP=lv_label_create(scr); lv_label_set_text(capP,tr(T_POWER));
  lv_obj_set_style_text_color(capP,lv_color_hex(0x9aa0a6),0); lv_obj_set_pos(capP,8,98);
  bmPower=lv_btnmatrix_create(scr); lv_btnmatrix_set_map(bmPower,mapPower);
  lv_obj_set_pos(bmPower,8,114); lv_obj_set_size(bmPower,304,36);
  lv_obj_add_event_cb(bmPower,evPower,LV_EVENT_VALUE_CHANGED,nullptr);

  // Operating mode (5 modes, 2 rows) + On/Off (right, spanning both rows)
  mapMode[0]=tr(T_COMFORT); mapMode[1]=tr(T_NIGHT); mapMode[2]=tr(T_TURBO); mapMode[3]="\n";
  mapMode[4]=tr(T_AUTO);    mapMode[5]=tr(T_MANUAL); mapMode[6]="";
  bmMode=lv_btnmatrix_create(scr); lv_btnmatrix_set_map(bmMode,mapMode);
  lv_obj_set_pos(bmMode,8,156); lv_obj_set_size(bmMode,236,62);
  lv_obj_set_style_pad_all(bmMode,3,0);
  lv_obj_add_event_cb(bmMode,evMode,LV_EVENT_VALUE_CHANGED,nullptr);
  btnOnOff=mkBtn(scr,tr(T_ON),248,156,64,62,evOnOff);
  lblOnOff=lv_obj_get_child(btnOnOff,0);

  lblPhase=lv_label_create(scr); lv_label_set_text(lblPhase,"--");
  lv_obj_set_style_text_color(lblPhase,lv_color_hex(0x9aa0a6),0);
  lv_obj_align(lblPhase,LV_ALIGN_BOTTOM_LEFT,8,-2);
}

// ==== Phase 4: system menu screens ========================================
static lv_obj_t *scrMain, *scrMenu, *scrOvens, *scrWifi, *scrMqtt, *scrInfo, *scrFan;
static lv_obj_t *bmFan, *btnSilent, *lblSilent, *btnFanEntry, *lblFanEntry;
static lv_obj_t *ovList, *ovStatus;
static lv_obj_t *wifiList, *wifiSsidLbl, *taWifiPass, *kbWifi, *wifiStatus;
static lv_obj_t *taHost, *taPort, *taUser, *taPass, *kbMqtt, *infoLbl;
static int g_ovShown = -1, g_wifiScanState = 0;
static void startTouchCalibration();   // forward declaration (used by the menu)

static void navMenu(lv_event_t*){ lv_scr_load(scrMenu); }
static void navMain(lv_event_t*){ lv_scr_load(scrMain); }
static void navFan (lv_event_t*){ lv_scr_load(scrFan); }
static void navBackScan(lv_event_t*){ bleScanListStop(); lv_scr_load(scrMenu); }

static lv_obj_t* mkTitle(lv_obj_t* scr, const char* t){
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
  lv_obj_t* l=lv_label_create(scr); lv_label_set_text(l,t);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(l,lv_color_hex(0xF0C000),0);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, 6); return l;
}
static void mkBackBtn(lv_obj_t* scr, lv_event_cb_t cb){
  lv_obj_t* b=lv_btn_create(scr); lv_obj_set_size(b,84,36); lv_obj_align(b,LV_ALIGN_BOTTOM_RIGHT,-6,-6);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,tr(T_BACK)); lv_obj_center(l);
}
// Couple keyboard to text field (show on focus, hide on OK/loss of focus)
static void taEvent(lv_event_t* e){
  lv_event_code_t code=lv_event_get_code(e);
  lv_obj_t* ta=lv_event_get_target(e); lv_obj_t* kb=(lv_obj_t*)lv_event_get_user_data(e);
  if(code==LV_EVENT_FOCUSED){ lv_keyboard_set_textarea(kb,ta); lv_obj_clear_flag(kb,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(kb); }
  else if(code==LV_EVENT_DEFOCUSED){ lv_obj_add_flag(kb,LV_OBJ_FLAG_HIDDEN); }
}
static void kbEvent(lv_event_t* e){ lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN); }
static lv_obj_t* mkTextArea(lv_obj_t* scr, const char* ph, lv_coord_t y, bool pass, lv_obj_t* kb){
  lv_obj_t* ta=lv_textarea_create(scr); lv_textarea_set_one_line(ta,true);
  lv_textarea_set_placeholder_text(ta,ph); if(pass) lv_textarea_set_password_mode(ta,true);
  lv_obj_set_width(ta,304); lv_obj_align(ta,LV_ALIGN_TOP_LEFT,8,y);
  lv_obj_add_event_cb(ta, taEvent, LV_EVENT_FOCUSED, kb);
  lv_obj_add_event_cb(ta, taEvent, LV_EVENT_DEFOCUSED, kb);
  return ta;
}

// -- Stove scan --
static void ovenPick(lv_event_t* e){
  int i=(int)(intptr_t)lv_event_get_user_data(e); String mac,name;
  if(bleScanListGet(i,mac,name)){
    g_cfg.targetMac=mac; bleScanListStop();
    Serial.printf(">> Stove selected: %s -> saving to NVS + restart\n", mac.c_str());
    configReboot();                 // persist immediately (save + restart) -> connect to chosen oven
    return;                         // (matches serial 'target'; no separate Save step needed)
  }
  bleScanListStop(); lv_scr_load(scrMenu);
}
static void ovenRescan(lv_event_t*){ g_ovShown=-1; bleScanListStart(); }
static void buildOvens(){
  scrOvens=lv_obj_create(NULL); mkTitle(scrOvens,tr(T_FIND));
  lv_obj_t* b=lv_btn_create(scrOvens); lv_obj_set_size(b,96,32); lv_obj_align(b,LV_ALIGN_TOP_RIGHT,-6,6);
  lv_obj_add_event_cb(b,ovenRescan,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* bl=lv_label_create(b); lv_label_set_text(bl,tr(T_RESCAN)); lv_obj_center(bl);
  ovStatus=lv_label_create(scrOvens); lv_obj_set_style_text_color(ovStatus,lv_color_hex(0x9aa0a6),0);
  lv_label_set_text(ovStatus,tr(T_SEARCHING)); lv_obj_align(ovStatus,LV_ALIGN_TOP_LEFT,8,40);
  ovList=lv_list_create(scrOvens); lv_obj_set_size(ovList,304,146); lv_obj_align(ovList,LV_ALIGN_TOP_LEFT,8,58);
  mkBackBtn(scrOvens, navBackScan);
}
static void ovensRefresh(){
  if(lv_scr_act()!=scrOvens) return;
  lv_label_set_text(ovStatus, bleScanListBusy()?tr(T_SEARCHING):(bleScanListCount()?"":tr(T_NOTHING)));
  int n=bleScanListCount(); if(n==g_ovShown) return; g_ovShown=n; lv_obj_clean(ovList);
  for(int i=0;i<n;i++){ String mac,name; bleScanListGet(i,mac,name);
    lv_obj_t* it=lv_list_add_btn(ovList, LV_SYMBOL_GPS, (name+"  "+mac).c_str());
    lv_obj_add_event_cb(it, ovenPick, LV_EVENT_CLICKED, (void*)(intptr_t)i); }
}

// -- WiFi --
static void wifiPick(lv_event_t* e){
  const char* ssid=lv_list_get_btn_text(wifiList, lv_event_get_target(e));
  g_cfg.wifiSsid=ssid; lv_label_set_text(wifiSsidLbl,(String("SSID: ")+ssid).c_str());
}
static void wifiScanStart(lv_event_t*){ WiFi.scanNetworks(true); g_wifiScanState=1; lv_label_set_text(wifiStatus,tr(T_SEARCHING)); }
static void wifiSave(lv_event_t*){ g_cfg.wifiPass=lv_textarea_get_text(taWifiPass); lv_scr_load(scrMenu); }
static void buildWifi(){
  scrWifi=lv_obj_create(NULL); mkTitle(scrWifi,"WiFi");
  lv_obj_t* b=lv_btn_create(scrWifi); lv_obj_set_size(b,70,32); lv_obj_align(b,LV_ALIGN_TOP_RIGHT,-6,6);
  lv_obj_add_event_cb(b,wifiScanStart,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* bl=lv_label_create(b); lv_label_set_text(bl,tr(T_SCAN)); lv_obj_center(bl);
  wifiStatus=lv_label_create(scrWifi); lv_obj_set_style_text_color(wifiStatus,lv_color_hex(0x9aa0a6),0);
  lv_label_set_text(wifiStatus,""); lv_obj_align(wifiStatus,LV_ALIGN_TOP_LEFT,8,40);
  wifiList=lv_list_create(scrWifi); lv_obj_set_size(wifiList,304,84); lv_obj_align(wifiList,LV_ALIGN_TOP_LEFT,8,56);
  wifiSsidLbl=lv_label_create(scrWifi); lv_label_set_text(wifiSsidLbl,"SSID: -"); lv_obj_align(wifiSsidLbl,LV_ALIGN_TOP_LEFT,8,144);
  kbWifi=lv_keyboard_create(scrWifi); lv_obj_add_flag(kbWifi,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kbWifi,kbEvent,LV_EVENT_READY,nullptr); lv_obj_add_event_cb(kbWifi,kbEvent,LV_EVENT_CANCEL,nullptr);
  taWifiPass=mkTextArea(scrWifi,tr(T_WIFIPASS),164,true,kbWifi);
  lv_obj_t* sv=lv_btn_create(scrWifi); lv_obj_set_size(sv,90,36); lv_obj_align(sv,LV_ALIGN_BOTTOM_LEFT,6,-6);
  lv_obj_add_event_cb(sv,wifiSave,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* svl=lv_label_create(sv); lv_label_set_text(svl,tr(T_SAVE)); lv_obj_center(svl);
  mkBackBtn(scrWifi, navMenu);
}
static void wifiRefresh(){
  if(lv_scr_act()!=scrWifi || g_wifiScanState!=1) return;
  int n=WiFi.scanComplete();
  if(n==WIFI_SCAN_RUNNING) return;
  g_wifiScanState=0; lv_label_set_text(wifiStatus, n>0?"":tr(T_NONETS)); lv_obj_clean(wifiList);
  for(int i=0;i<n && i<25;i++){ lv_obj_t* it=lv_list_add_btn(wifiList,LV_SYMBOL_WIFI,WiFi.SSID(i).c_str());
    lv_obj_add_event_cb(it,wifiPick,LV_EVENT_CLICKED,nullptr); }
  WiFi.scanDelete();
}

// -- MQTT --
static void mqttSave(lv_event_t*){
  g_cfg.mqttHost=lv_textarea_get_text(taHost); g_cfg.mqttPort=atoi(lv_textarea_get_text(taPort));
  g_cfg.mqttUser=lv_textarea_get_text(taUser); g_cfg.mqttPass=lv_textarea_get_text(taPass);
  lv_scr_load(scrMenu);
}
static void buildMqtt(){
  scrMqtt=lv_obj_create(NULL); mkTitle(scrMqtt,"MQTT");
  kbMqtt=lv_keyboard_create(scrMqtt); lv_obj_add_flag(kbMqtt,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kbMqtt,kbEvent,LV_EVENT_READY,nullptr); lv_obj_add_event_cb(kbMqtt,kbEvent,LV_EVENT_CANCEL,nullptr);
  taHost=mkTextArea(scrMqtt,tr(T_HOST),34,false,kbMqtt);
  taPort=mkTextArea(scrMqtt,tr(T_PORT),72,false,kbMqtt);
  taUser=mkTextArea(scrMqtt,tr(T_USER),110,false,kbMqtt);
  taPass=mkTextArea(scrMqtt,tr(T_PASS),148,true,kbMqtt);
  lv_obj_t* sv=lv_btn_create(scrMqtt); lv_obj_set_size(sv,90,36); lv_obj_align(sv,LV_ALIGN_BOTTOM_LEFT,6,-6);
  lv_obj_add_event_cb(sv,mqttSave,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* svl=lv_label_create(sv); lv_label_set_text(svl,tr(T_SAVE)); lv_obj_center(svl);
  mkBackBtn(scrMqtt, navMenu);
}
static void mqttFormLoad(){
  lv_textarea_set_text(taHost,g_cfg.mqttHost.c_str());
  char p[8]; snprintf(p,sizeof(p),"%u",g_cfg.mqttPort); lv_textarea_set_text(taPort,p);
  lv_textarea_set_text(taUser,g_cfg.mqttUser.c_str()); lv_textarea_set_text(taPass,g_cfg.mqttPass.c_str());
}

// -- Info --
static void buildInfo(){
  scrInfo=lv_obj_create(NULL); mkTitle(scrInfo,tr(T_INFO));
  infoLbl=lv_label_create(scrInfo); lv_obj_set_width(infoLbl,304); lv_label_set_long_mode(infoLbl,LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(infoLbl,lv_color_hex(0xE0E0E0),0); lv_obj_align(infoLbl,LV_ALIGN_TOP_LEFT,8,42);
  mkBackBtn(scrInfo, navMenu);
}
static void infoRefresh(){
  String ip = WiFi.isConnected()? WiFi.localIP().toString() : String("-");
  String s = String(tr(T_MAC)) + ": " + (g_ovenMac.length()?g_ovenMac:String("-")) + "\n"
           + String(tr(T_SERIAL)) + ": " + (g_ovenSerial.length()?g_ovenSerial:String("-")) + "\n"
           + "BLE: "      + String(g_oven.bleOnline?"online":"offline") + "\n"
           + "WiFi: "     + (WiFi.isConnected()?g_cfg.wifiSsid:String("-")) + "  " + ip + "\n"
           + "MQTT: "     + g_cfg.mqttHost + ":" + String(g_cfg.mqttPort);
  lv_label_set_text(infoLbl, s.c_str());
}

// -- Display & Language --
static lv_obj_t *scrSettings, *ddLang, *ddTimeout;
static const uint16_t TO_SECS[6] = {0,30,60,120,300,600};
static void settingsLoad(){
  lv_dropdown_set_selected(ddLang, g_cfg.lang<=2? g_cfg.lang : 0);
  uint8_t ti=0; for(uint8_t i=0;i<6;i++) if(TO_SECS[i]==g_cfg.dispTimeoutS) ti=i;
  lv_dropdown_set_selected(ddTimeout, ti);
}
static void settingsSave(lv_event_t*){
  uint8_t nl = lv_dropdown_get_selected(ddLang);
  g_cfg.dispTimeoutS = TO_SECS[lv_dropdown_get_selected(ddTimeout)];
  bool langChanged = (nl != g_cfg.lang); g_cfg.lang = nl;
  configSave(); g_lastTouch=millis();
  if(langChanged) ESP.restart(); else lv_scr_load(scrMenu);   // language -> restart
}
static void buildSettings(){
  scrSettings=lv_obj_create(NULL); mkTitle(scrSettings, tr(T_SETTINGS));
  lv_obj_t* l1=lv_label_create(scrSettings); lv_label_set_text(l1,tr(T_LANGUAGE));
  lv_obj_set_style_text_color(l1,lv_color_hex(0x9aa0a6),0); lv_obj_align(l1,LV_ALIGN_TOP_LEFT,8,44);
  ddLang=lv_dropdown_create(scrSettings); lv_dropdown_set_options(ddLang,"English\nDeutsch\nItaliano");
  lv_obj_set_width(ddLang,180); lv_obj_align(ddLang,LV_ALIGN_TOP_LEFT,8,64);
  lv_obj_t* l2=lv_label_create(scrSettings); lv_label_set_text(l2,tr(T_DISPTO));
  lv_obj_set_style_text_color(l2,lv_color_hex(0x9aa0a6),0); lv_obj_align(l2,LV_ALIGN_TOP_LEFT,8,116);
  ddTimeout=lv_dropdown_create(scrSettings);
  String opt=String(tr(T_OFFOPT))+"\n30 s\n1 min\n2 min\n5 min\n10 min";
  lv_dropdown_set_options(ddTimeout,opt.c_str());
  lv_obj_set_width(ddTimeout,180); lv_obj_align(ddTimeout,LV_ALIGN_TOP_LEFT,8,136);
  lv_obj_t* sv=lv_btn_create(scrSettings); lv_obj_set_size(sv,110,36); lv_obj_align(sv,LV_ALIGN_BOTTOM_LEFT,6,-6);
  lv_obj_add_event_cb(sv,settingsSave,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* svl=lv_label_create(sv); lv_label_set_text(svl,tr(T_SAVE)); lv_obj_center(svl);
  mkBackBtn(scrSettings, navMenu);
}

// -- Main menu --
static void mnOvens(lv_event_t*){ g_ovShown=-1; bleScanListStart(); lv_scr_load(scrOvens); }
static void mnWifi (lv_event_t*){ lv_scr_load(scrWifi); }
static void mnMqtt (lv_event_t*){ mqttFormLoad(); lv_scr_load(scrMqtt); }
static void mnInfo (lv_event_t*){ infoRefresh(); lv_scr_load(scrInfo); }
static void mnCalib(lv_event_t*){ startTouchCalibration(); }
static void mnSettings(lv_event_t*){ settingsLoad(); lv_scr_load(scrSettings); }
static void mnReboot(lv_event_t*){ configReboot(); }
// -- Fan & Silent (control sub-screen, reachable from the main screen) --
static void buildFan(){
  scrFan=lv_obj_create(NULL); mkTitle(scrFan,tr(T_FAN));
  // Fan level: Auto + 1..5 (one row)
  mapFan[0]=tr(T_AUTO); mapFan[1]="1"; mapFan[2]="2"; mapFan[3]="3"; mapFan[4]="4"; mapFan[5]="5"; mapFan[6]="";
  bmFan=lv_btnmatrix_create(scrFan); lv_btnmatrix_set_map(bmFan,mapFan);
  lv_obj_set_size(bmFan,304,56); lv_obj_align(bmFan,LV_ALIGN_TOP_LEFT,8,54);
  lv_obj_add_event_cb(bmFan,evFan,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_t* cap=lv_label_create(scrFan); lv_label_set_text(cap,tr(T_FAN));
  lv_obj_set_style_text_color(cap,lv_color_hex(0x9aa0a6),0); lv_obj_align(cap,LV_ALIGN_TOP_LEFT,8,40);
  // Silent toggle
  lv_obj_t* capS=lv_label_create(scrFan); lv_label_set_text(capS,tr(T_SILENT));
  lv_obj_set_style_text_color(capS,lv_color_hex(0x9aa0a6),0); lv_obj_align(capS,LV_ALIGN_TOP_LEFT,8,124);
  btnSilent=lv_btn_create(scrFan); lv_obj_set_size(btnSilent,140,48); lv_obj_align(btnSilent,LV_ALIGN_TOP_LEFT,8,142);
  lv_obj_add_event_cb(btnSilent,evSilent,LV_EVENT_CLICKED,nullptr);
  lblSilent=lv_label_create(btnSilent); lv_label_set_text(lblSilent,tr(T_SILENT)); lv_obj_center(lblSilent);
  mkBackBtn(scrFan, navMain);
}
static void buildMenu(){
  scrMenu=lv_obj_create(NULL); mkTitle(scrMenu,tr(T_SYSTEM));
  lv_obj_t* list=lv_list_create(scrMenu); lv_obj_set_size(list,304,192); lv_obj_align(list,LV_ALIGN_TOP_LEFT,8,36);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_GPS,      tr(T_FIND)),     mnOvens,   LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_WIFI,     "WiFi"),         mnWifi,    LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_UPLOAD,   tr(T_MQTT)),     mnMqtt,    LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_SETTINGS, tr(T_SETTINGS)), mnSettings,LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_LIST,     tr(T_INFO)),     mnInfo,    LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_EDIT,     tr(T_CALIB)),    mnCalib,   LV_EVENT_CLICKED,nullptr);
  lv_obj_add_event_cb(lv_list_add_btn(list,LV_SYMBOL_POWER,    tr(T_SAVEREB)),  mnReboot,  LV_EVENT_CLICKED,nullptr);
  mkBackBtn(scrMenu, navMain);
}

// -- Touch calibration (2-point, per device -> NVS) --
static lv_obj_t *scrCalib, *calibCross, *calibInfo;
static int  g_calibStep = -1, g_calRawX[2], g_calRawY[2], g_calLastX, g_calLastY, g_calCount = 0;
static bool g_calPressed = false;
static const lv_point_t CAL_PTS[2] = {{25,25}, {SCR_W-25, SCR_H-25}};
static void buildCalib(){
  scrCalib=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrCalib, lv_color_hex(0x0d1117),0);
  calibInfo=lv_label_create(scrCalib); lv_obj_set_style_text_color(calibInfo,lv_color_hex(0xffffff),0);
  lv_obj_set_style_text_align(calibInfo,LV_TEXT_ALIGN_CENTER,0);
  lv_label_set_text(calibInfo,tr(T_CALTITLE));
  lv_obj_align(calibInfo, LV_ALIGN_CENTER, 0, 0);
  calibCross=lv_label_create(scrCalib); lv_label_set_text(calibCross, LV_SYMBOL_PLUS);
  lv_obj_set_style_text_font(calibCross,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(calibCross,lv_color_hex(0xF0C000),0);
}
static void placeCross(int i){ lv_obj_align(calibCross, LV_ALIGN_TOP_LEFT, CAL_PTS[i].x-13, CAL_PTS[i].y-16); }
static void startTouchCalibration(){
  g_calibStep=0; g_calPressed=false; g_calCount=0; placeCross(0); lv_scr_load(scrCalib);
}
static void calibTick(){
  if(g_calibStep<0) return;
  if(ts.touched()){ TS_Point p=ts.getPoint(); g_calLastX=p.x; g_calLastY=p.y; g_calPressed=true; g_calCount++; return; }
  if(!(g_calPressed && g_calCount>=3)){ g_calPressed=false; g_calCount=0; return; }  // no clean tap
  g_calRawX[g_calibStep]=g_calLastX; g_calRawY[g_calibStep]=g_calLastY;
  g_calibStep++; g_calPressed=false; g_calCount=0;
  if(g_calibStep<2){ placeCross(g_calibStep); return; }
  float sx0=CAL_PTS[0].x, sx1=CAL_PTS[1].x, sy0=CAL_PTS[0].y, sy1=CAL_PTS[1].y;
  float dxr=(g_calRawX[1]-g_calRawX[0])/(sx1-sx0), dyr=(g_calRawY[1]-g_calRawY[0])/(sy1-sy0);
  g_cfg.tsMinX=(int16_t)lroundf(g_calRawX[0]-sx0*dxr);
  g_cfg.tsMaxX=(int16_t)lroundf(g_calRawX[0]+(SCR_W-sx0)*dxr);
  g_cfg.tsMinY=(int16_t)lroundf(g_calRawY[0]-sy0*dyr);
  g_cfg.tsMaxY=(int16_t)lroundf(g_calRawY[0]+(SCR_H-sy0)*dyr);
  g_cfg.touchCalibrated=true; configSave();
  Serial.printf(">> Touch calibrated: X %d..%d  Y %d..%d\n", g_cfg.tsMinX,g_cfg.tsMaxX,g_cfg.tsMinY,g_cfg.tsMaxY);
  g_calibStep=-1; lv_scr_load(scrMain);
}
static void buildScreens(){ buildMenu(); buildOvens(); buildWifi(); buildMqtt(); buildInfo(); buildSettings(); buildCalib(); buildFan(); }

// ---- DisplayUI implementation -------------------------------------------
namespace {
class CydDisplay : public DisplayUI {
public:
  void begin() override {
    tft.init();
    tft.setRotation(1);                       // landscape 320x240
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);   // backlight on
    g_lastTouch = millis();

    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    ts.begin();
    ts.setRotation(1);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, lvbuf, nullptr, SCR_W * LV_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCR_W;
    disp_drv.ver_res  = SCR_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    buildUI();
    scrMain = lv_scr_act();     // remember the main screen
    buildScreens();             // create Menu/Stove/WiFi/MQTT/Info
    // Gear -> system menu (on the main screen)
    lv_obj_t* gear=lv_btn_create(scrMain); lv_obj_set_size(gear,44,32);   // like the fan icon
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -6, 2);
    lv_obj_set_style_pad_all(gear,2,0);
    lv_obj_add_event_cb(gear, navMenu, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* gl=lv_label_create(gear); lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_28, 0);            // symbol size = fan
    lv_obj_center(gl);
    // Fan/Silent quick access (top, between title and Bluetooth symbol) - fan icon
    btnFanEntry=lv_btn_create(scrMain); lv_obj_set_size(btnFanEntry,44,32);
    lv_obj_align(btnFanEntry, LV_ALIGN_TOP_RIGHT, -52, 2);   // directly left of the gear
    lv_obj_set_style_pad_all(btnFanEntry,2,0);
    lv_obj_add_event_cb(btnFanEntry, navFan, LV_EVENT_CLICKED, nullptr);
    lblFanEntry=lv_img_create(btnFanEntry); lv_img_set_src(lblFanEntry, &fan_icon); lv_obj_center(lblFanEntry);
    lv_obj_set_style_img_recolor(lblFanEntry, lv_color_hex(0xEAEAEA), 0);
    lv_obj_set_style_img_recolor_opa(lblFanEntry, LV_OPA_COVER, 0);
    if(!g_cfg.touchCalibrated) startTouchCalibration();   // first boot: calibrate first
    Serial.printf(">> CYD/LVGL init ok. FreeHeap=%u\n", ESP.getFreeHeap());
  }

  void tick() override {
    static uint32_t last = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last); last = now;
    lv_timer_handler();
    calibTick();                // touch calibration (only active during calibration)
    ovensRefresh();             // refresh scan lists (they internally check the active screen)
    wifiRefresh();
    // Screensaver: backlight off after inactivity (not during calibration).
    // IMPORTANT: use fresh millis() (not the 'now' cached above) - lv_timer_handler()
    // may have set g_lastTouch to a later time in the meantime -> otherwise
    // unsigned underflow and false switch-off on tap.
    uint16_t to = g_cfg.dispTimeoutS;
    if (to>0 && !g_screenOff && g_calibStep<0 && (millis()-g_lastTouch) > (uint32_t)to*1000){
      digitalWrite(TFT_BL, LOW); g_screenOff=true;
    }
  }

  void render(const OvenState& s) override {
    char b[16];
    // Apply detected capabilities once: fan matrix = Auto + 1..fanLevels, hide fan
    // entry if the stove has no controllable fan.
    static bool capsApplied = false;
    if (!g_caps.detected) capsApplied = false;      // re-apply after (re)connect
    else if (!capsApplied){
      capsApplied = true;
      int n = g_caps.fanLevels; if(n<1)n=1; if(n>5)n=5;
      static const char* dg[6] = {"","1","2","3","4","5"};
      mapFan[0]=tr(T_AUTO);
      for(int i=1;i<=n;i++) mapFan[i]=dg[i];
      mapFan[n+1]="";
      lv_btnmatrix_set_map(bmFan, mapFan);
      if(g_caps.fanCount==0) lv_obj_add_flag(btnFanEntry, LV_OBJ_FLAG_HIDDEN);
      else                   lv_obj_clear_flag(btnFanEntry, LV_OBJ_FLAG_HIDDEN);
    }
    if (!isnan(s.roomC))     { snprintf(b,sizeof(b),"%.1f", s.roomC);     lv_label_set_text(lblRoom, b); }
    if (!isnan(s.setpointC)) { snprintf(b,sizeof(b),"%.1f", s.setpointC); lv_label_set_text(lblSet,  b); }
    // mark active power
    for (uint16_t i=0;i<5;i++)
      if ((int)i == s.power-1) lv_btnmatrix_set_btn_ctrl  (bmPower,i,LV_BTNMATRIX_CTRL_CHECKED);
      else                     lv_btnmatrix_clear_btn_ctrl(bmPower,i,LV_BTNMATRIX_CTRL_CHECKED);
    // mark active operating mode (register value -> UI button index)
    int uiSel=-1; for(int i=0;i<5;i++) if(MODE_UI2REG[i]==s.mode) uiSel=i;
    for (uint16_t i=0;i<5;i++)
      if ((int)i == uiSel)     lv_btnmatrix_set_btn_ctrl  (bmMode,i,LV_BTNMATRIX_CTRL_CHECKED);
      else                     lv_btnmatrix_clear_btn_ctrl(bmMode,i,LV_BTNMATRIX_CTRL_CHECKED);
    // On/Off (button shows the ACTION): running -> "OFF", otherwise "ON"
    bool run = (s.phase == 3);
    lv_label_set_text(lblOnOff, run ? tr(T_OFF) : tr(T_ON));   // button shows the ACTION
    lv_obj_set_style_bg_color(btnOnOff, lv_color_hex(run?0x2ea043:0x6e7681), 0);
    // Fan sub-screen: mark live level (0x0324=1..5; Auto not detectable from actual value)
    for (uint16_t i=0;i<=5;i++)
      if ((int)i == s.fanLevel) lv_btnmatrix_set_btn_ctrl  (bmFan,i,LV_BTNMATRIX_CTRL_CHECKED);
      else                      lv_btnmatrix_clear_btn_ctrl(bmFan,i,LV_BTNMATRIX_CTRL_CHECKED);
    bool sil = (s.flags>=0) && ((s.flags>>5)&1);              // color the Silent toggle
    lv_obj_set_style_bg_color(btnSilent, lv_color_hex(sil?0x2ea043:0x6e7681), 0);
    if (s.state >= 0){                          // fine phase 0x0320 (name from capture)
      const char* sn = stateName(s.state);
      char ph[48];
      if (sn[0]) snprintf(ph,sizeof(ph),"%s: %s", tr(T_PHASE), sn);
      else       snprintf(ph,sizeof(ph),"%s: 0x%04X", tr(T_PHASE), (unsigned)s.state);
      lv_label_set_text(lblPhase, ph);
    } else if (s.phase >= 0){                    // coarse fallback until 0x0320 is available
      char ph[48];
      snprintf(ph,sizeof(ph),"%s: %s", tr(T_PHASE), s.phase==3?tr(T_RUN):s.phase==1?tr(T_STOP):"?");
      lv_label_set_text(lblPhase, ph);
    }
    // Bluetooth indicator: blue=connected, yellow=connection lost/reconnect, red=no connection.
    // On boot with never any data received (lastRxMs==0) -> red. Yellow only after a dropout.
    uint32_t col;
    if (s.bleOnline)                                       col = 0x2f81f7;  // blue: connected
    else if (s.lastRxMs && (millis()-s.lastRxMs) < 20000)  col = 0xF0C000;  // yellow: reconnect
    else                                                   col = 0xda3633;  // red:  no connection
    lv_label_set_text(lblConn, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(lblConn, lv_color_hex(col), 0);
  }
};
CydDisplay g_cyd;
}

DisplayUI& displayInstance(){ return g_cyd; }

#endif  // USE_DISPLAY
