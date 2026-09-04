#include "cardinfo.h"

uint8_t cardInfo[CARD_SIZE];

bool cardInserted = false;
bool cardHasData = false;

void clearCardData()
{
    memset(cardInfo, 0, CARD_SIZE);

    cardInserted = false;
    cardHasData = false;
}