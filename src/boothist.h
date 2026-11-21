// boothist.h — historique persistant des raisons de demarrage (PATCHES.md §19).
// Tampon circulaire de 10 codes esp_reset_reason() en NVS (namespace "boothist") + compteur de boots croissant.
#pragma once
#include <Arduino.h>

const char* bootReasonName(int code);   // "poweron", "brownout", "panic", ...
void   bootHistRecord();                // setup() : ajoute la raison du boot courant (UNE ecriture NVS)
uint32_t bootHistCount();               // nombre total de boots enregistres (ne fait qu'augmenter)
String bootHistString();                // "sw,brownout,poweron" plus recents d'abord ("" si vide)
void   bootHistPrint();                 // affichage serie
void   bootHistClear();                 // vide l'historique et le compteur
