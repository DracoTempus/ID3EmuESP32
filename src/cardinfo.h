#pragma once
//Card info I found somewhere, but I forgot now.  In some random forum.
//But it is 69 bytes across 3 tracks which gives us 207 bytes



#include <Arduino.h>

constexpr size_t TRACK_SIZE = 69;
constexpr size_t NUM_TRACKS = 3;
constexpr size_t CARD_SIZE  = TRACK_SIZE * NUM_TRACKS;

extern uint8_t cardInfo[CARD_SIZE];

extern bool cardInserted;
extern bool cardHasData;

void clearCardData();


