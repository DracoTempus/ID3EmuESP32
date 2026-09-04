#include <Arduino.h>
#include <LittleFS.h>

#include "cardreader.h"
#include "cardinfo.h"
#include "webhost.h"
#include "nfc.h"

static constexpr int LOCAL_ONLY_PIN = 13;

void setupStorage()
{
    if (!LittleFS.begin(true))
    {
        Serial.println("failed to mount");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP32 Initial D ver 3 card reader emulator");

    pinMode(LOCAL_ONLY_PIN, INPUT_PULLUP);

    bool localOnly =
        digitalRead(LOCAL_ONLY_PIN) == LOW;

    setDatabaseEnabled(!localOnly);

    nfcInit();
    clearCardData();
    webInit();
    cardReaderInit();

    Serial.printf(
        "Card storage mode: %s\n",
        localOnly ? "LOCAL ONLY" : "LOCAL + DATABASE"
    );

    Serial.printf(
    "Flash size: %u bytes\n",
    ESP.getFlashChipSize()
    );
    setupStorage();
}

void loop()
{
    // nfcHardwareTest();
    // delay(10);

    cardReaderHandle();
    nfcHandle();
    webHandle();
}