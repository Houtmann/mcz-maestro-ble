// appconfig.h — runtime-changeable, persistent configuration (NVS/Preferences).
// Replaces the previous compile-time macros as config.h only
// provides the default/first-boot values. These values are changed via the display system
// menu and permanently stored with configReboot() (followed by a restart → clean re-init).
#pragma once
#include <Arduino.h>

struct AppConfig {
  String   wifiSsid;
  String   wifiPass;
  String   mqttHost;
  uint16_t mqttPort = 1883;
  String   mqttUser;
  String   mqttPass;
  String   targetMac;   // Oven BLE MAC (with/without ':'); empty = first available MCZ_EP
  // Touch calibration (resistive, differs per panel/device)
  int16_t  tsMinX = 220, tsMaxX = 3760, tsMinY = 350, tsMaxY = 3790;
  bool     touchCalibrated = false;   // false -> first-boot calibration
  // Display & language
  uint8_t  lang = 0;                   // 0=EN, 1=DE, 2=IT
  uint16_t dispTimeoutS = 120;         // Screensaver: backlight off after x s (0=never)
};
extern AppConfig g_cfg;

void configLoad();    // load from NVS; missing values -> defaults from config.h
void configSave();    // write g_cfg to NVS
void configReboot();  // configSave() + ESP.restart()
