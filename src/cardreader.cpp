#include <Arduino.h>

#include "cardreader.h"
#include "cardinfo.h"
#include "webhost.h"
#include "nfc.h"

static constexpr uint8_t STX  = 0x02;
static constexpr uint8_t ETX  = 0x03;
static constexpr uint8_t ENQ  = 0x05;
static constexpr uint8_t ACK  = 0x06;
static constexpr uint8_t NACK = 0x15;

constexpr uint8_t RESULT_OK = 0x30;
constexpr uint8_t RESULT_READ_MISFIRE = 0x31;
constexpr uint8_t RESULT_WRITE_MISFIRE = 0x32;
constexpr uint8_t RESULT_ILLEGAL_OP = 0x38;
constexpr uint8_t RESULT_SYSTEM_ERROR = 0x41;

//I added these results, but never use.
constexpr uint8_t RESULT_PRINT_ERR = 0x35;
constexpr uint8_t RESULT_BATTERY_ERR = 0x40;
constexpr uint8_t RESULT_TRACK_1_READ_ERR = 0x51;
constexpr uint8_t RESULT_TRACK_2_READ_ERR = 0x52;
constexpr uint8_t RESULT_TRACK_3_READ_ERR = 0x53;
constexpr uint8_t RESULT_TRACK_1_AND_2_READ_ERR = 0x54;
constexpr uint8_t RESULT_TRACK_1_AND_3_READ_ERR = 0x55;
constexpr uint8_t RESULT_TRACK_2_AND_3_READ_ERR = 0x56;

constexpr uint8_t STATE_IDLING = 0x30;
constexpr uint8_t STATE_ILLEGAL_OP = 0x32;
constexpr uint8_t STATE_DRIVING = 0x33;
constexpr uint8_t STATE_WAITING_FOR_CARD = 0x34;

//STATES I DONT USE.  Infinite cards
constexpr uint8_t STATE_DISPENSER_EMPTY = 0x35;
constexpr uint8_t STATE_NO_DISPENSER = 0x36;
constexpr uint8_t STATE_CARD_FULL = 0x37;

bool readerInitialized = false;

// Initial D loads font slots x01 through x14 hex.
static constexpr uint8_t FONT_SLOT_COUNT = 20;
uint32_t fontSlotsSeen = 0;
uint32_t diagWritePackets = 0;
uint32_t diagWriteCommands = 0;
uint32_t diagBadChecksums = 0;
uint32_t diagBadEtq = 0;
uint32_t diagRxPackets = 0;
uint32_t diagRx53 = 0;
uint32_t diagRx7C = 0;
uint32_t diagRx20 = 0;
uint32_t diagFontRx = 0;
uint32_t diagFontCommands = 0;

uint8_t diagLastRxCommand = 0;
unsigned long diagLastRxAt = 0;

static constexpr int ESP_RX_PIN = 16;
static constexpr int ESP_TX_PIN = 17;

namespace {
    char pendingPrintText[128];
    size_t pendingPrintTextLength = 0;
    bool pendingPrintLog = false;
    HardwareSerial CardSerial(2);

    bool returnToNoCardAfterResponse = false;

    uint8_t responseData[220];
    size_t responseDataLength = 0;

    uint8_t resultStatus = RESULT_OK;
    uint8_t stateStatus = STATE_IDLING;

    uint8_t rxBuffer[260];
    size_t rxLength = 0;

    uint8_t currentCommand = 0;
    uint8_t commandParams[220];
    size_t commandParamLength = 0;

    bool commandActive = false;
    int commandStep = 0;

    enum class CardPosition : uint8_t
    {
        NO_CARD,
        EJECT,
        READ_WRITE,
        THERMAL,
        DISPENSER
    };

    CardPosition cardPosition = CardPosition::NO_CARD;

    // --YACardEmu--
    // CRP-1231BR uses:
    // NO CARD       = 00000
    // EJECT         = 00001
    // READ/WRITE    = 11000
    // THERMAL       = 00111
    // DISPENSER     = 11100
    const uint8_t positionValues[] =
    {
        0b00000,
        0b00001,
        0b11000,
        0b00111,
        0b11100
    };

    bool shutterOpen = true;

    uint8_t getPositionByte()
    {
        uint8_t result = 0;

        // C1231BR shutter state
        result |= shutterOpen ? 0x80 : 0x40;

        // YACardEmu reports dispenser available
        result |= 0x20;

        result |= positionValues[
            static_cast<uint8_t>(cardPosition)
        ];

        return result;
    }
    void capturePrintText()
    {
        pendingPrintTextLength = 0;

        for (
            size_t i = 0;
            i < commandParamLength &&
            pendingPrintTextLength < sizeof(pendingPrintText) - 1;
            i++
        )
        {
            uint8_t c = commandParams[i];

            // Keep normal printable ASCII.
            if (c >= 0x20 && c <= 0x7E)
            {
                pendingPrintText[
                    pendingPrintTextLength++
                ] = static_cast<char>(c);
            }
            else
            {
                // Separate printable sections so strings
                // don't get mashed together.
                if (
                    pendingPrintTextLength > 0 &&
                    pendingPrintText[
                        pendingPrintTextLength - 1
                    ] != ' '
                )
                {
                    pendingPrintText[
                        pendingPrintTextLength++
                    ] = ' ';
                }
            }
        }

        // Remove trailing spaces.
        while (
            pendingPrintTextLength > 0 &&
            pendingPrintText[
                pendingPrintTextLength - 1
            ] == ' '
        )
        {
            pendingPrintTextLength--;
        }

        pendingPrintText[
            pendingPrintTextLength
        ] = '\0';
    }

    uint8_t countLoadedFonts()
    {
        uint8_t count = 0;

        for (uint8_t slot = 1; slot <= FONT_SLOT_COUNT; slot++)
        {
            uint32_t bit =
                1UL << (slot - 1);

            if (fontSlotsSeen & bit)
                count++;
        }

        return count;
    }
    void printHex(const char* prefix, const uint8_t* data, size_t length)
    {
        Serial.print(prefix);

        for (size_t i = 0; i < length; i++)
        {
            if (data[i] < 0x10)
                Serial.print("0");

            Serial.print(data[i], HEX);
            Serial.print(" ");
        }

        ////Serial.println();
    }

    void sendResponse()
    {
        uint8_t tx[260];
        size_t pos = 0;

        tx[pos++] = STX;

        // command + position + RESULT + STATE + data + ETX + checksum-count rule
        uint8_t count =
            4 +
            responseDataLength +
            2;

        tx[pos++] = count;

        uint8_t checksum = count;

        tx[pos++] = currentCommand;
        checksum ^= currentCommand;

        uint8_t position = getPositionByte();

        tx[pos++] = position;
        checksum ^= position;

        tx[pos++] = resultStatus;
        checksum ^= resultStatus;

        tx[pos++] = stateStatus;
        checksum ^= stateStatus;

        for (size_t i = 0; i < responseDataLength; i++)
        {
            tx[pos++] = responseData[i];
            checksum ^= responseData[i];
        }

        tx[pos++] = ETX;
        checksum ^= ETX;

        tx[pos++] = checksum;

        CardSerial.write(tx, pos);
        CardSerial.flush();

        // printHex("CARD TX: ", tx, pos);
    }

    void appendTrack(int track)
    {
        if (track < 0 || track >= 3)
            return;

        const size_t offset = track * TRACK_SIZE;

        for (size_t i = 0; i < TRACK_SIZE; i++)
        {
            responseData[responseDataLength++] =
                cardInfo[offset + i];
        }
    }

    void handleWriteCard()
    {
        diagWriteCommands++;
        if (!cardInserted)
        {
            resultStatus = RESULT_ILLEGAL_OP;
            stateStatus = STATE_IDLING;
            commandActive = false;
            return;
        }

        if (commandParamLength < 3)
        {
            resultStatus = RESULT_SYSTEM_ERROR;
            stateStatus = STATE_IDLING;
            commandActive = false;
            return;
        }

        uint8_t mode  = commandParams[0];
        uint8_t parity = commandParams[1];
        uint8_t track = commandParams[2];

        if (mode != 0x30)
        {
            Serial.println("Unsupported write mode");

            resultStatus = RESULT_ILLEGAL_OP;
            stateStatus = STATE_IDLING;
            commandActive = false;
            return;
        }

        const uint8_t* data = &commandParams[3];
        size_t dataLength = commandParamLength - 3;

        switch (track)
        {
            // Track 1
            case 0x30:
                if (dataLength >= TRACK_SIZE)
                    memcpy(&cardInfo[0], data, TRACK_SIZE);
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Track 2
            case 0x31:
                if (dataLength >= TRACK_SIZE)
                    memcpy(&cardInfo[TRACK_SIZE], data, TRACK_SIZE);
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Track 3
            case 0x32:
                if (dataLength >= TRACK_SIZE)
                    memcpy(&cardInfo[TRACK_SIZE * 2], data, TRACK_SIZE);
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Tracks 1 + 2
            case 0x33:
                if (dataLength >= TRACK_SIZE * 2)
                    memcpy(&cardInfo[0], data, TRACK_SIZE * 2);
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Tracks 1 + 3
            case 0x34:
                if (dataLength >= TRACK_SIZE * 2)
                {
                    memcpy(&cardInfo[0], data, TRACK_SIZE);
                    memcpy(
                        &cardInfo[TRACK_SIZE * 2],
                        data + TRACK_SIZE,
                        TRACK_SIZE
                    );
                }
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Tracks 2 + 3
            case 0x35:
                if (dataLength >= TRACK_SIZE * 2)
                    memcpy(
                        &cardInfo[TRACK_SIZE],
                        data,
                        TRACK_SIZE * 2
                    );
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            // Tracks 1 + 2 + 3
            case 0x36:
                if (dataLength >= CARD_SIZE)
                    memcpy(cardInfo, data, CARD_SIZE);
                else
                    resultStatus = RESULT_SYSTEM_ERROR;
                break;

            default:
                Serial.printf(
                    "Unknown write track selector: %02X\n",
                    track
                );

                resultStatus = RESULT_ILLEGAL_OP;
                break;
        }

        if (resultStatus == RESULT_OK)
        {
            cardHasData = true;

        }

        stateStatus = STATE_IDLING;
        commandActive = false;
    }

    void handleReadCard()
    {
        if (commandStep == 0)
        {
            stateStatus = STATE_DRIVING;
            return;
        }

        if (!cardInserted)
        {
            stateStatus = STATE_WAITING_FOR_CARD;

            nfcStartListening();

            return;
        }

        nfcStopListening();

        if (commandParamLength < 3)
        {
            resultStatus = RESULT_SYSTEM_ERROR;
            stateStatus = STATE_IDLING;
            commandActive = false;
            return;
        }

        uint8_t mode  = commandParams[0];
        uint8_t track = commandParams[2];

        responseDataLength = 0;

        cardPosition = CardPosition::READ_WRITE;

        if (mode == 0x32)
        {
            stateStatus = STATE_IDLING;
            commandActive = false;
            return;
        }

        switch (track)
        {
            case 0x30:
                appendTrack(0);
                break;

            case 0x31:
                appendTrack(1);
                break;

            case 0x32:
                appendTrack(2);
                break;

            case 0x33:
                appendTrack(0);
                appendTrack(1);
                break;

            case 0x34:
                appendTrack(0);
                appendTrack(2);
                break;

            case 0x35:
                appendTrack(1);
                appendTrack(2);
                break;

            case 0x36:
                appendTrack(0);
                appendTrack(1);
                appendTrack(2);
                break;

            default:
                resultStatus = RESULT_ILLEGAL_OP;
                break;
        }

        stateStatus = STATE_IDLING;
        commandActive = false;
    }

    void processCommand()
    {
        if (!commandActive)
            return;

        switch (currentCommand)
        {
            case 0x10:
            {

                uint8_t mode = 0x30;
                if (commandParamLength > 0)
                {
                    mode = commandParams[0];
                }

                readerInitialized = true;
                fontSlotsSeen = 0;

                shutterOpen = true;
                if (cardInserted)
                {
                    if (mode == 0x31)
                        cardPosition = CardPosition::EJECT;
                }
                else
                {
                    cardPosition = CardPosition::NO_CARD;
                }

                returnToNoCardAfterResponse = false;

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;

                responseDataLength = 0;
                commandActive = false;

                break;
            }
            case 0x40:
            {
                ////Serial.println("0x40 - Cancel");
                nfcStopListening();

                resultStatus = RESULT_OK;
                stateStatus = STATE_ILLEGAL_OP;

                responseDataLength = 0;
                commandActive = false;

                break;
            }
            case 0xB0:
            {
                resultStatus = RESULT_OK;
                responseDataLength = 0;

                switch (commandStep)
                {
                    // YACardEmu does nothing on the first
                    // processing step. Reader is still busy.
                    case 0:
                    {
                        stateStatus = STATE_DRIVING;
                        commandActive = true;
                        break;
                    }

                    // Actually dispense the new card.
                    case 1:
                    {
                        if (cardInserted)
                        {
                            resultStatus = RESULT_ILLEGAL_OP;
                            stateStatus = STATE_IDLING;
                            commandActive = false;
                            break;
                        }

                        cardInserted = true;
                        cardHasData = false;

                        memset(
                            cardInfo,
                            0,
                            CARD_SIZE
                        );

                        shutterOpen = false;

                        // Card is coming out of the dispenser,
                        // not magically at the R/W head yet.
                        cardPosition =
                            CardPosition::DISPENSER;

                        stateStatus =
                            STATE_DRIVING;

                        commandActive = true;

                        break;
                    }

                    // Move dispensed card into magnetic
                    // read/write position.
                    case 2:
                    {
                        cardPosition =
                            CardPosition::READ_WRITE;

                        stateStatus =
                            STATE_DRIVING;

                        commandActive = true;

                        break;
                    }

                    // Mechanical operation has completed.
                    default:
                    {
                        stateStatus =
                            STATE_IDLING;

                        commandActive = false;

                        break;
                    }
                }

                break;
            }
            case 0x53:
            {
                if (currentCommand == 0x53)
                {
                    diagWritePackets++;
                }

                handleWriteCard();

                break;
            }
            case 0x7A:
            {
                diagFontCommands++;
                uint8_t slot = 0xFF;

                if (commandParamLength > 0)
                {
                    slot = commandParams[0];

                    if (
                        slot >= 0x01 &&
                        slot <= 0x14
                    )
                    {
                        fontSlotsSeen |=
                            1UL << (slot - 1);
                    }
                }

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;

                responseDataLength = 0;
                commandActive = false;

                break;
            }

            //I tried to pass just finished, because im not printing.  but it didn't work.  I am not sure why, too fast?
            case 0x7C:
            {
                resultStatus = RESULT_OK;
                responseDataLength = 0;

                switch (commandStep)
                {
                    case 0:
                    {
                        // Save what Initial D wants printed.
                        // Do NOT Serial.print it yet.
                        capturePrintText();

                        if (!cardInserted)
                        {
                            resultStatus = RESULT_PRINT_ERR;
                            stateStatus = STATE_IDLING;
                            commandActive = false;

                            pendingPrintLog = true;

                            break;
                        }

                        // Simulate card moving to thermal print head.
                        cardPosition =
                            CardPosition::THERMAL;

                        stateStatus =
                            STATE_DRIVING;

                        commandActive = true;

                        break;
                    }

                    case 1:
                    {
                        // Simulate returning card to magnetic
                        // read/write position.
                        cardPosition =
                            CardPosition::READ_WRITE;

                        stateStatus =
                            STATE_DRIVING;

                        commandActive = true;

                        break;
                    }

                    default:
                    {
                        // Print operation is now complete.
                        stateStatus =
                            STATE_IDLING;

                        commandActive = false;

                        // Safe to log after sendResponse().
                        pendingPrintLog = true;

                        break;
                    }
                }

                break;
            }

            //EJECT command i think
            case 0x80:
            {
                ////Serial.println("0x80 - Eject Card");

                cardInserted = false;
                cardPosition = CardPosition::NO_CARD;
                shutterOpen = true;

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;

                commandActive = false;
                break;
            }
            // Read Status
            case 0x20:
            {
                ////Serial.println("0x20 - Read Status");

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;
                commandActive = false;
                break;
            }

            // Read Data 2
            case 0x33:
            {
                ////Serial.println("0x33 - Read Card");
                handleReadCard();
                break;
            }

            case 0xC0:
            {
                ////Serial.println("0xC0 - LED");
                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;
                commandActive = false;
                //Would be cool to add an LED just return good though
                break;
            }

            case 0xD0:
            {
                //////Serial.println("0xD0 - Shutter Open/Close");

                if (commandParamLength > 0)
                {
                    if (commandParams[0] == 0x31)
                        shutterOpen = true;
                    else if (commandParams[0] == 0x30)
                        shutterOpen = false;
                }

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;
                commandActive = false;
                break;
            }

            case 0xF0:
            {
                ////Serial.println("0xF0 - Version");
                static const char version[] =
                    "AP:S1234-5678,OS:S9012-3456,0000";

                responseDataLength = 0;

                for (size_t i = 0; i < strlen(version); i++)
                    responseData[responseDataLength++] = version[i];

                resultStatus = RESULT_OK;
                stateStatus = STATE_IDLING;
                commandActive = false;
                break;
            }

            default:
            {
                Serial.printf(
                    "UNKNOWN COMMAND: 0x%02X\n",
                    currentCommand
                );

                stateStatus = STATE_ILLEGAL_OP;
                commandActive = false;
                break;
            }
        }

        commandStep++;
    }

    void processReceivedPacket()
    {
        if (rxLength < 8)
        {
            ////Serial.println("short packet");
            rxLength = 0;
            return;
        }

        const size_t packetLength = rxLength;
        uint8_t count = rxBuffer[1];

        if (rxBuffer[count] != ETX)
        {
            CardSerial.write(NACK);
            CardSerial.flush();

            ////Serial.println("Bad ETX");

            rxLength = 0;
            return;
        }

        uint8_t checksum = count;

        for (size_t i = 2; i <= count; i++)
        {
            checksum ^= rxBuffer[i];
        }

        uint8_t receivedChecksum =
            rxBuffer[count + 1];

        if (checksum != receivedChecksum)
        {
            // NACK FIRST -- debug later.
            CardSerial.write(NACK);
            CardSerial.flush();

            Serial.printf(
                "BAD CHECKSUM expected=%02X received=%02X\n",
                checksum,
                receivedChecksum
            );

            rxLength = 0;
            return;
        }

        currentCommand = rxBuffer[2];
        if (currentCommand == 0x7A)
            diagFontRx++;
        diagRxPackets++;
        diagLastRxCommand = currentCommand;
        diagLastRxAt = millis();

        if (currentCommand == 0x53)
            diagRx53++;

        if (currentCommand == 0x7C)
            diagRx7C++;

        if (currentCommand == 0x20)
            diagRx20++;

        commandParamLength = 0;

        size_t payloadLength =
            count - 2;

        size_t parameterCount = 0;

        if (payloadLength > 4)
        {
            parameterCount =
                payloadLength - 4;

            if (parameterCount > sizeof(commandParams))
            {
                CardSerial.write(NACK);
                CardSerial.flush();

                rxLength = 0;
                return;
            }
        }

        if (currentCommand == 0x53)
        {
            unsigned long ackStart = micros();

            //Weird timing on this.  Still has a problem sometimes.
            delayMicroseconds(2300);

            CardSerial.write(ACK);
            CardSerial.flush();
        }
        else
        {
            //Its about 2ms.  But you know, I was born in 86 so.
            delayMicroseconds(1986);
        }
        CardSerial.write(ACK);
        CardSerial.flush();


        //Doing stuff that takes too much time during responses.
        if (parameterCount > 0)
        {
            memcpy(
                commandParams,
                &rxBuffer[6],
                parameterCount
            );
        }

        commandParamLength =
            parameterCount;

        resultStatus = RESULT_OK;
        stateStatus = STATE_DRIVING;

        responseDataLength = 0;

        commandStep = 0;
        commandActive = true;

        rxLength = 0;
    }
}

void insertCardHandle()
{
    cardInserted = true;
    cardPosition = CardPosition::EJECT;
}

void removeCardHandle()
{
    cardInserted = false;
    cardPosition = CardPosition::NO_CARD;
}


void cardReaderInit()
{
    CardSerial.begin(
        9600,
        SERIAL_8E1,
        ESP_RX_PIN,
        ESP_TX_PIN
    );

    Serial.println("CRP-1231BR emulator");
    Serial.println("UART2: 9600 8E1");
    Serial.printf("RX GPIO: %d\n", ESP_RX_PIN);
    Serial.printf("TX GPIO: %d\n", ESP_TX_PIN);
}

void cardReaderHandle()
{
    static unsigned long lastRxByteUs = 0;

    for (;;)
    {
        if (!CardSerial.available())
        {
            if (rxLength == 0)
                break;

            // Do priority work before recheck queue
            if (
                micros() - lastRxByteUs >
                5000
            )
            {
                rxLength = 0;
                break;
            }

            delayMicroseconds(50);
            continue;
        }

        uint8_t b =
            static_cast<uint8_t>(
                CardSerial.read()
            );

        lastRxByteUs = micros();

        //ENQ handling
        if (
            rxLength == 0 &&
            b == ENQ
        )
        {
            uint8_t completedCommand = currentCommand;
            bool commandWasActive = commandActive;
            bool cardWasInserted = cardInserted;

            processCommand();
            sendResponse();

            if (
                commandWasActive &&
                completedCommand == 0x80 &&
                cardWasInserted &&
                cardHasData
            )
            {
                queueCardUpload();
            }

            if (currentCommand == 0x53)
            {
                Serial.printf(
                    "WRITE DIAG packets=%lu commands=%lu result=%02X state=%02X\n",
                    diagWritePackets,
                    diagWriteCommands,
                    resultStatus,
                    stateStatus
                );
            }

            if (returnToNoCardAfterResponse)
            {
                cardPosition =
                    CardPosition::NO_CARD;

                returnToNoCardAfterResponse =
                    false;
            }

            continue;
        }

        if (
            rxLength == 0 &&
            b != STX
        )
        {
            continue;
        }

        if (
            rxLength >=
            sizeof(rxBuffer)
        )
        {
            rxLength = 0;
            continue;
        }

        rxBuffer[rxLength++] = b;

        if (rxLength >= 2)
        {
            size_t expectedLength =
                static_cast<size_t>(
                    rxBuffer[1]
                ) + 2;

            if (
                rxLength ==
                expectedLength
            )
            {
                processReceivedPacket();
            }
        }

        static uint32_t lastPrintedRxPackets = 0;

        if (
            diagRxPackets != lastPrintedRxPackets &&
            diagLastRxAt != 0 &&
            millis() - diagLastRxAt > 500
        )
        {
            lastPrintedRxPackets =
                diagRxPackets;

            //So many problems this just got so long, doesn't add much so kept.
            Serial.printf(
                "[IDLE DIAG] last=%02X total=%lu 20=%lu 7C=%lu 53RX=%lu 53ENQ=%lu 53WRITE=%lu 7ARX=%lu 7AENQ=%lu fonts=%u badETX=%lu badCRC=%lu\n",
                diagLastRxCommand,
                diagRxPackets,
                diagRx20,
                diagRx7C,
                diagRx53,
                diagWritePackets,
                diagWriteCommands,
                diagFontRx,
                diagFontCommands,
                countLoadedFonts(),
                diagBadEtq,
                diagBadChecksums
            );
        }
    }
}

void cardReaderSendRaw(
    const uint8_t* data,
    size_t length
)
{
    if (data == nullptr || length == 0)
        return;

    CardSerial.write(data, length);
    CardSerial.flush();

    Serial.print("MANUAL TX: ");

    for (size_t i = 0; i < length; i++)
    {
        Serial.printf("%02X ", data[i]);
    }
}

void debug_CardReaderSendLoopbackTest()
{
    static const uint8_t test[] =
    {
        0x55,
        0xAA,
        0x12,
        0x34,
        0x02,
        0x07,
        0x10
    };

    cardReaderSendRaw(
        test,
        sizeof(test)
    );
}

bool CardReaderIsInitialized()
{
    return readerInitialized;
}

uint8_t CardReaderFontsLoaded()
{
    return countLoadedFonts();
}

bool CardReaderFontsComplete()
{
    return countLoadedFonts() >= FONT_SLOT_COUNT;
}

const char* CardReaderGetStateText()
{
    if (!readerInitialized)
    {
        return "Waiting for Initialize";
    }

    if (!CardReaderFontsComplete())
    {
        return "Loading Fonts";
    }

    if (cardInserted)
    {
        return "Card Inserted";
    }

    if (cardHasData)
    {
        return "Ready - Card Stored";
    }

    return "Ready";
}