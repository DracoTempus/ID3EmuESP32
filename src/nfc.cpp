#include <Arduino.h>

#include <PN532_HSU.h>
#include <PN532.h>

#include "nfc.h"
#include "webhost.h"


static constexpr int NFC_RX_PIN = 32;
static constexpr int NFC_TX_PIN = 33;
static constexpr size_t PLAYER_ID_MAX = 128;

HardwareSerial NfcSerial(1);

PN532_HSU pn532Hsu(NfcSerial, NFC_RX_PIN, NFC_TX_PIN);
PN532 pn532(pn532Hsu);


static volatile bool nfcReady = false;
static volatile bool nfcListening = false;


// ------------------------------------------
// Android HCE protocol
//
// AID:
// F0 01 02 03 04 05 06
//
// GET ID:
// 80 CA 00 00 00
// ------------------------------------------

static const uint8_t SELECT_APP_APDU[] =
{
    0x00,
    0xA4,
    0x04,
    0x00,
    0x07,

    0xF0,
    0x01,
    0x02,
    0x03,
    0x04,
    0x05,
    0x06,

    0x00
};


static const uint8_t GET_PLAYER_ID_APDU[] =
{
    0x80,
    0xCA,
    0x00,
    0x00,
    0x00
};


// ------------------------------------------
// We let the NFC work happen on core 0.
//
// This is important because PN532 polling can
// take time and we DO NOT want it holding up
// the NAOMI UART code.
// ------------------------------------------

static TaskHandle_t nfcTaskHandle = nullptr;


static portMUX_TYPE nfcMux =
    portMUX_INITIALIZER_UNLOCKED;


static bool pendingPlayerIdReady = false;
static char pendingPlayerId[PLAYER_ID_MAX + 1] = {0};


// ------------------------------------------
// APDU result helper
// ------------------------------------------

static bool apduSucceeded(
    const uint8_t* data,
    uint8_t length
)
{
    if (length < 2)
        return false;

    return
        data[length - 2] == 0x90 &&
        data[length - 1] == 0x00;
}


static bool isAmiiboTag(uint8_t uidLength)
{
    if (uidLength != 7)
        return false;

    uint8_t idPage1[4];
    uint8_t idPage2[4];
    uint8_t lockPage[4];

    if (!pn532.mifareultralight_ReadPage(0x15, idPage1))
        return false;

    if (!pn532.mifareultralight_ReadPage(0x16, idPage2))
        return false;

    if (!pn532.mifareultralight_ReadPage(0x82, lockPage))
        return false;

    // Amiibo identification block format version.
    if (idPage2[3] != 0x02)
        return false;

    // NTAG215 dynamic lock bytes used by amiibo.
    if (lockPage[0] != 0x01 ||
        lockPage[1] != 0x00 ||
        lockPage[2] != 0x0F)
        return false;

    return true;
}

static bool makeAmiiboId(
    const uint8_t* uid,
    uint8_t uidLength,
    char* output,
    size_t outputSize)
{
    static const char hex[] = "0123456789ABCDEF";

    // "AMIIBO" + 14 UID hex chars + null
    if (outputSize < 6 + (uidLength * 2) + 1)
        return false;

    strcpy(output, "AMIIBO");
    size_t pos = 6;

    for (uint8_t i = 0; i < uidLength; i++)
    {
        output[pos++] = hex[(uid[i] >> 4) & 0x0F];
        output[pos++] = hex[uid[i] & 0x0F];
    }

    output[pos] = '\0';
    return true;
}

// ------------------------------------------
// Read ID from Android HCE app
// ------------------------------------------

static bool readAndroidPlayerId(char* output, size_t outputSize)
{
    if (output == nullptr || outputSize < 2)
        return false;

    uint8_t uid[7] = {0};
    uint8_t uidLength = 0;

    if (!pn532.readPassiveTargetID(
        PN532_MIFARE_ISO14443A,
        uid,
        &uidLength,
        100,
        true))
    {
        return false;
    }

    for (uint8_t i = 0; i < uidLength; i++)
        Serial.printf("%02X", uid[i]);
    Serial.println();

    // Physical amiibo / NTAG215.
    if (isAmiiboTag(uidLength))
    {
        bool success = makeAmiiboId(
            uid,
            uidLength,
            output,
            outputSize
        );

        pn532.inRelease();

        if (success)
        {
            Serial.println(output);
        }

        return success;
    }

    // Otherwise try our Android HCE application.
    uint8_t response[PLAYER_ID_MAX + 2];
    uint8_t responseLength = sizeof(response);

    Serial.println("NFC: trying Android HCE");

    bool success = pn532.inDataExchange(
        const_cast<uint8_t*>(SELECT_APP_APDU),
        sizeof(SELECT_APP_APDU),
        response,
        &responseLength
    );

    if (!success || !apduSucceeded(response, responseLength))
    {
        pn532.inRelease();
        return false;
    }

    // SELECT response is: ASCII player ID + 90 00
    size_t dataLength = responseLength - 2;
    size_t outputPos = 0;

    for (size_t i = 0;
         i < dataLength && outputPos < outputSize - 1;
         i++)
    {
        uint8_t c = response[i];

        if (c >= 0x20 && c <= 0x7E)
            output[outputPos++] = static_cast<char>(c);
    }

    output[outputPos] = '\0';

    Serial.print("NFC: ID received = ");
    Serial.println(output);

    pn532.inRelease();

    return outputPos > 0;
}


// ------------------------------------------
// NFC background task
// ------------------------------------------

static void nfcTask(void*)
{
    for (;;)
    {
        if (
            !nfcReady ||
            !nfcListening
        )
        {
            vTaskDelay(
                pdMS_TO_TICKS(25)
            );

            continue;
        }


        char playerId[PLAYER_ID_MAX + 1] = {0};


        if (readAndroidPlayerId(playerId,sizeof(playerId)))
        {
            // Stop scanning immediately.
            nfcListening = false;


            portENTER_CRITICAL(&nfcMux);

            strncpy(
                pendingPlayerId,
                playerId,
                sizeof(pendingPlayerId) - 1
            );

            pendingPlayerId[sizeof(pendingPlayerId) - 1] = '\0';

            pendingPlayerIdReady = true;

            portEXIT_CRITICAL(&nfcMux);
        }


        vTaskDelay(
            pdMS_TO_TICKS(25)
        );
    }
}


static bool samConfigFixed()
{
    uint8_t command[4] = {
        PN532_COMMAND_SAMCONFIGURATION,
        0x01,
        0x14,
        0x01
    };

    int8_t writeResult = pn532Hsu.writeCommand(command, 4);

    if (writeResult != 0)
    {
        Serial.printf("SAMConfig write failed: %d\n", writeResult);
        return false;
    }

    uint8_t response[128];
    int16_t readResult = pn532Hsu.readResponse(response, sizeof(response), 1000);

    Serial.printf("SAMConfig response length: %d\n", readResult);

    return readResult >= 0;
}

// ------------------------------------------
// Initialize PN532
// ------------------------------------------

void nfcInit()
{
    pn532.begin();

    uint32_t version = pn532.getFirmwareVersion();

    if (!version)
    {
        Serial.println("NFC: PN532 not found");
        nfcReady = false;
        return;
    }

    Serial.printf(
        "NFC: PN532 found, firmware %u.%u\n",
        (version >> 16) & 0xFF,
        (version >> 8) & 0xFF
    );

    Serial.println("NFC: configuring SAM...");

    if (!samConfigFixed())
    {
        Serial.println("NFC: SAMConfig failed");
        nfcReady = false;
        return;
    }

    Serial.println("NFC: SAMConfig OK");

    if (pn532.setPassiveActivationRetries(0x01))
        Serial.println("NFC: passive retries OK");
    else
        Serial.println("NFC: passive retries failed");

    nfcReady = true;

    Serial.printf(
        "NFC: PN532 ready on RX=%d TX=%d\n",
        NFC_RX_PIN,
        NFC_TX_PIN
    );

    xTaskCreatePinnedToCore(
        nfcTask,
        "NFC",
        4096,
        nullptr,
        1,
        &nfcTaskHandle,
        0
    );
}


// ------------------------------------------
// Called by card reader when Initial D
// starts waiting for a card.
// ------------------------------------------

void nfcStartListening()
{
    if (!nfcReady)
    {
        Serial.println("NFC: cannot listen - PN532 not ready");
        return;
    }

    if (!nfcListening)
    {
        nfcListening = true;
        Serial.println("NFC: ON - listening for phone/amiibo");
    }
}

void nfcStopListening()
{
    if (nfcListening)
    {
        nfcListening = false;
        Serial.println("NFC: OFF");
    }
}

bool nfcIsListening()
{
    return nfcListening;
}


// ------------------------------------------
// Called from normal Arduino loop.
//
// This receives completed IDs from the
// background NFC task.
// ------------------------------------------

void nfcHandle()
{
    char playerId[PLAYER_ID_MAX + 1] = {0};

    bool gotId = false;


    portENTER_CRITICAL(
        &nfcMux
    );

    if (pendingPlayerIdReady)
    {
        strncpy(
            playerId,
            pendingPlayerId,
            sizeof(playerId) - 1
        );

        pendingPlayerIdReady = false;

        gotId = true;
    }

    portEXIT_CRITICAL(
        &nfcMux
    );


    if (!gotId)
        return;


    String id =
        String(playerId);

    id.trim();


    if (id.length() == 0)
        return;


    Serial.print("NFC PLAYER ID: ");
    Serial.println(id);

    setCurrentID(id);
    loadCardForCurrentID();
}

void nfcHardwareTest()
{
    static bool initialized = false;
    static unsigned long lastScan = 0;

    if (!initialized)
    {
        initialized = true;

        Serial.println("========== PN532 HARDWARE TEST ==========");

        pn532.begin();

        Serial.println("TEST 1: requesting PN532 firmware...");

        uint32_t version = pn532.getFirmwareVersion();

        if (!version)
        {
            Serial.println("FAIL: PN532 did not respond");
            return;
        }

        Serial.println("PASS: PN532 responded");

        Serial.printf("Chip: PN5%02X\n", (version >> 24) & 0xFF);
        Serial.printf(
            "Firmware: %u.%u\n",
            (version >> 16) & 0xFF,
            (version >> 8) & 0xFF
        );

        Serial.println("TEST 2: SAMConfig...");

        if (samConfigFixed())
            Serial.println("PASS: SAMConfig");
        else
        {
            Serial.println("FAIL: SAMConfig");
            return;
        }

        pn532.setPassiveActivationRetries(0x01);
        Serial.println("Tap an amiibo, NFC card, or phone...");
    }

    if (millis() - lastScan < 250)
        return;

    lastScan = millis();

    uint8_t uid[10] = {0};
    uint8_t uidLength = 0;

    bool found = pn532.readPassiveTargetID(
        PN532_MIFARE_ISO14443A,
        uid,
        &uidLength,
        100
    );

    if (!found)
        return;

    Serial.println();
    Serial.println("========== NFC TARGET FOUND ==========");
    Serial.printf("UID length: %u bytes\n", uidLength);

    Serial.print("UID: ");
    for (uint8_t i = 0; i < uidLength; i++)
        Serial.printf("%02X", uid[i]);

    Serial.println();
    Serial.println("======================================");

    delay(1000);
}