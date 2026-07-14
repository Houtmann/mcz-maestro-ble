// ble_api.h — thin interface of the BLE layer (main.cpp) for the display menu.
// Oven search: collects visible MCZ_EP devices WITHOUT connecting automatically.
#pragma once
#include <Arduino.h>

void bleScanListStart();                 // disconnect active connection, start collection scan
void bleScanListStop();                  // leave collection mode (normal auto-connect active again)
bool bleScanListBusy();                  // is a scan running right now?
int  bleScanListCount();                 // number of ovens found
bool bleScanListGet(int i, String& mac, String& name);
