#pragma once
#include <Arduino.h>

void nfcInit();
void nfcHandle();

void nfcStartListening();
void nfcStopListening();

bool nfcIsListening();

void nfcHardwareTest();