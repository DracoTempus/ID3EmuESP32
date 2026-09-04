#pragma once
// CRP-1231BR-10 emulator
//
// Initial D Arcade Stage Ver.3
//
// Serial config:
//   9600 baud
//   8 data bits
//   Even parity
//   1 stop bit

#include <Arduino.h>

void insertCardHandle();
void removeCardHandle();

void cardReaderInit();
void cardReaderHandle();

// Web Stuff
bool CardReaderIsInitialized();
bool CardReaderFontsComplete();
uint8_t CardReaderFontsLoaded();
const char* CardReaderGetStateText();

void cardReaderSendRaw(
    const uint8_t* data,
    size_t length
);

//TESTING SHIT FUNCTIONS.  Mainly for debugging and I kept breaking stuff.

void debug_CardReaderSendLoopbackTest();