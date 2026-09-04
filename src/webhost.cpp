#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>

#include "webhost.h"
#include "cardinfo.h"
#include "cardreader.h"

static const char* WIFI_SSID = "TheFonggrasinFamily";

static WebServer server(80);
static String db = "192.168.137.163:5050";
static String currentID = "";
static bool databaseEnabled = true;

static bool cardUploadPending = false;
static unsigned long lastCardUploadAttempt = 0;
static unsigned long cardUploadQueuedAt = 0;
static bool cardLocalSavedForPendingUpload = false;

namespace {
    const char INDEX_HTML[] PROGMEM = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">

        <title>ESP32 Card Emulator</title>

        <style>
            body {
                font-family: Arial, sans-serif;
                background: #111827;
                color: #f3f4f6;
                margin: 0;
                padding: 40px 20px;
            }

            .container {
                max-width: 700px;
                margin: auto;
            }

            .card {
                background: #1f2937;
                border-radius: 12px;
                padding: 24px;
                margin-bottom: 20px;
            }

            h1 {
                margin-top: 0;
            }

            .status {
                color: #4ade80;
                font-weight: bold;
            }

            code {
                background: #111827;
                padding: 4px 8px;
                border-radius: 4px;
            }
            .file-row {
                display: flex;
                justify-content: space-between;
                align-items: center;
                gap: 12px;
                padding: 8px 0;
                border-bottom: 1px solid #374151;
            }

            .file-name {
                word-break: break-all;
            }

            .delete-button {
                background: #dc2626;
                color: white;
                border: 0;
                padding: 6px 10px;
                border-radius: 6px;
                cursor: pointer;
            }
        </style>
    </head>
    <body>
    <div class="container">
        <h1>ESP32 Card Emulator</h1>
        <div class="card">
            <h2>Card Reader Status</h2>

            <p>
                Reader:
                <span id="readerState" class="status pending">
                    Waiting...
                </span>
            </p>

            <p>
                Initialized:
                <span id="initialized" class="status pending">
                    UNKNOWN
                </span>
            </p>

            <p>
                Fonts:
                <span id="fonts" class="status pending">
                    0 / 20
                </span>
            </p>

            <p>
                Font Setup:
                <span id="fontsComplete" class="status pending">
                    INCOMPLETE
                </span>
            </p>
        </div>

        <div class="card">
            <h2>Card Status</h2>
            <p>
                Card Data:
                <span id="cardData" class="status pending" >
                    NONE
                </span>
            </p>

            <p>
                Card Inserted:
                <span id="cardInserted" class="status pending" >
                    NO
                </span>
            </p>
        </div>

        <div class="card">

            <h2>Player</h2>

            <p>Database</p>

            <input id="db" type="text" value="192.168.137.163:5050" placeholder="192.168.137.163:5050">

            <p>Player ID</p>

            <input id="playerId" type="text" placeholder="Draco" >

            <br><br>

            <button onclick="saveConfig()">
                Set
            </button>

            <button onclick="insertCard()">
                Insert Card
            </button>

            <button onclick="removeCard()">
                Remove Card
            </button>

            <p id="cardMessage"></p>

        </div>
        <div class="card">
            <h2>Local Cards</h2>

            <button onclick="saveCurrentCard()">
                Save Current
            </button>

            <br><br>

            <div id="localCards">Loading...</div>
        </div>
    </div>

    </body>
        <script>
            async function updateStatus()
                {
                    try
                    {
                        const response =        await fetch("/api/status");

                        const data =        await response.json();

                        document.getElementById(
                            "initialized"
                        ).textContent =        data.initialized
                                ? "YES"
                                : "NO";

                        document.getElementById(
                            "fonts"
                        ).textContent =        data.fontsLoaded +
                            " / 20";

                        document.getElementById(
                            "fontsComplete"
                        ).textContent =        data.fontsComplete
                                ? "COMPLETE"
                                : "INCOMPLETE";

                        document.getElementById(
                            "readerState"
                        ).textContent =        data.readerState;

                        document.getElementById(
                            "cardData"
                        ).textContent =        data.cardHasData
                                ? "STORED"
                                : "NONE";

                        document.getElementById(
                            "cardInserted"
                        ).textContent =        data.cardInserted
                                ? "YES"
                                : "NO";
                    }
                    catch (error)
                    {
                        console.log(error);
                    }
                }

            setInterval(updateStatus, 1000);
            updateStatus();
        async function saveConfig()
        {
            const db =
                document.getElementById("db").value;

            const id =
                document.getElementById("playerId").value;

            await fetch(
                "/api/config/set?db=" +
                encodeURIComponent(db) +
                "&id=" +
                encodeURIComponent(id)
            );

            document.getElementById(
                "cardMessage"
            ).textContent = "Configuration saved";
        }


        async function insertCard()
        {
            await saveConfig();

            const response =
                await fetch("/api/card/insert");

            const text =
                await response.text();

            if (text === "NO_CARD")
            {
                document.getElementById(
                    "cardMessage"
                ).textContent ="No existing card - new card can be purchased";
            }
            else
            {
                document.getElementById(
                    "cardMessage"
                ).textContent =text;
            }
        }


        async function removeCard()
        {
            const response =
                await fetch("/api/card/remove");

            document.getElementById(
                "cardMessage"
            ).textContent =
                await response.text();
        }
        async function loadLocalCards()
        {
            const container =
                document.getElementById("localCards");

            try
            {
                const response =
                    await fetch("/api/local/cards");

                const files =
                    await response.json();

                container.textContent = "";

                if (files.length === 0)
                {
                    container.textContent =
                        "No local cards.";
                    return;
                }

                for (const file of files)
                {
                    const row =
                        document.createElement("div");

                    row.className = "file-row";

                    const name =
                        document.createElement("span");

                    name.className = "file-name";
                    name.textContent =
                        file.name + " (" + file.size + " bytes)";

                    const button =
                        document.createElement("button");

                    button.className = "delete-button";
                    button.textContent = "Delete";

                    button.onclick = function()
                    {
                        deleteLocalCard(file.name);
                    };

                    row.appendChild(name);
                    row.appendChild(button);

                    container.appendChild(row);
                }
            }
            catch (error)
            {
                container.textContent =
                    "Failed to load local cards.";
            }
        }

        async function saveCurrentCard()
        {
            await fetch(
                "/api/local/card/savecurrent",
                {
                    method: "POST"
                }
            );

            await loadLocalCards();
        }
        async function deleteLocalCard(name)
        {
            if (!confirm("Delete " + name + "?"))
                return;

            const response = await fetch(
                "/api/local/card/delete?name=" +
                encodeURIComponent(name),
                {
                    method: "POST"
                }
            );

            if (!response.ok)
            {
                alert(await response.text());
                return;
            }

            await loadLocalCards();
        }

        setInterval(loadLocalCards, 3000);
        loadLocalCards();
        </script>
    </html>
    )rawliteral";

    enum class CardLookupResult : uint8_t
    {
        NONE,
        NO_CARD,
        FOUND,
        ERROR
    };

    void handleLocalCards()
    {
        String json = "[";
        bool first = true;

        File root = LittleFS.open("/");

        if (!root)
        {
            server.send(
                500,
                "application/json",
                "[]"
            );
            return;
        }

        File file = root.openNextFile();

        while (file)
        {
            if (!file.isDirectory())
            {
                String name = file.name();

                if (name.startsWith("/"))
                    name.remove(0, 1);

                if (name.endsWith(".ID3"))
                {
                    if (!first)
                        json += ",";

                    first = false;

                    json += "{\"name\":\"";
                    json += name;
                    json += "\",\"size\":";
                    json += String(file.size());
                    json += "}";
                }
            }

            file.close();
            file = root.openNextFile();
        }

        root.close();

        json += "]";

        server.send(
            200,
            "application/json",
            json
        );
    }

    void handleDeleteLocalCard()
    {
        if (!server.hasArg("name"))
        {
            server.send(
                400,
                "text/plain",
                "Missing file name"
            );
            return;
        }

        String name = server.arg("name");
        name.trim();

        if (name.startsWith("/"))
            name.remove(0, 1);

        if (
            name.length() == 0 ||
            name.indexOf('/') >= 0 ||
            name.indexOf('\\') >= 0 ||
            !name.endsWith(".ID3")
        )
        {
            server.send(
                400,
                "text/plain",
                "Invalid file name"
            );
            return;
        }

        String path = "/" + name;

        if (!LittleFS.exists(path))
        {
            server.send(
                404,
                "text/plain",
                "File not found"
            );
            return;
        }

        if (!LittleFS.remove(path))
        {
            server.send(
                500,
                "text/plain",
                "Delete failed"
            );
            return;
        }

        Serial.print("LOCAL CARD: deleted ");
        Serial.println(path);

        server.send(
            200,
            "text/plain",
            "DELETED"
        );
    }
    int hexNibble(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';

        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;

        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;

        return -1;
    }

    bool parseHex(
        const String& text,
        uint8_t* output,
        size_t maxLength,
        size_t& outputLength
    )
    {
        outputLength = 0;

        int highNibble = -1;

        for (size_t i = 0; i < text.length(); i++)
        {
            char c = text[i];

            // Allow spaces and common separators.
            if (
                c == ' '  ||
                c == '\t' ||
                c == ','  ||
                c == ':'  ||
                c == '-'
            )
            {
                continue;
            }

            int nibble = hexNibble(c);

            if (nibble < 0)
                return false;

            if (highNibble < 0)
            {
                highNibble = nibble;
            }
            else
            {
                if (outputLength >= maxLength)
                    return false;

                output[outputLength++] =static_cast<uint8_t>((highNibble << 4) | nibble);

                highNibble = -1;
            }
        }

        // Odd number of hex digits.
        if (highNibble >= 0)
            return false;

        return outputLength > 0;
    }


    String extractCardData(String response)
    {
        response.trim();

        // DB can simply return:
        //
        // 00
        //
        // or:
        //
        // 414 hex characters
        //
        if (response == "00")
            return "00";

        // Also accept JSON such as:
        //
        // {"id":"Draco","name":"DRACO","carddata":"ABC..."}
        //
        int key =
            response.indexOf("\"carddata\"");

        if (key >= 0)
        {
            int colon = response.indexOf(':', key);

            int firstQuote = response.indexOf('"', colon + 1);

            int secondQuote = response.indexOf('"',firstQuote + 1);

            if (firstQuote >= 0 && secondQuote > firstQuote)
            {
                return response.substring(
                    firstQuote + 1,
                    secondQuote
                );
            }
        }

        return response;
    }

    String urlEncode(const String& value)
    {
        String result;

        char temp[4];

        for (size_t i = 0; i < value.length(); i++)
        {
            char c = value[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            {
                result += c;
            }
            else
            {
                snprintf(temp,sizeof(temp),"%%%02X",static_cast<unsigned char>(c));
                result += temp;
            }
        }

        return result;
    }

    void handleRoot()
    {
        server.send_P(200,"text/html",INDEX_HTML);
    }

    String cardDataToHex()
    {
        String result;
        result.reserve(CARD_SIZE * 2);
        char temp[3];
        for (size_t i = 0; i < CARD_SIZE; i++)
        {
            snprintf(temp,sizeof(temp),"%02X",cardInfo[i]);
            result += temp;
        }

        return result;
    }

    String localCardPath(const String& id)
    {
        return "/" + id + ".ID3";
    }

    bool saveCardLocal()
    {
        if (!cardHasData)
            return false;

        String id = currentID;
        id.trim();

        if (id.length() == 0)
            id = "NOID";

        String path = localCardPath(id);

        File file = LittleFS.open(path, "w");

        if (!file)
        {
            Serial.print("LOCAL CARD: failed to open ");
            Serial.println(path);
            return false;
        }

        size_t written = file.write(cardInfo, CARD_SIZE);
        file.close();

        if (written != CARD_SIZE)
        {
            Serial.printf("LOCAL CARD: write failed %u/%u bytes\n",written,CARD_SIZE);
            return false;
        }

        Serial.printf("LOCAL CARD: saved %s (%u bytes)\n",path.c_str(),CARD_SIZE);

        return true;
    }

    bool loadCardLocal(const String& id)
    {
        String path = localCardPath(id);

        if (!LittleFS.exists(path))
        {
            Serial.print("LOCAL CARD: not found ");
            Serial.println(path);
            return false;
        }

        File file = LittleFS.open(path, "r");

        if (!file)
        {
            Serial.print("LOCAL CARD: failed to open ");
            Serial.println(path);
            return false;
        }

        if (file.size() != CARD_SIZE)
        {
            Serial.printf("LOCAL CARD: bad size %u expected %u\n",file.size(),CARD_SIZE);

            file.close();
            return false;
        }

        uint8_t data[CARD_SIZE];
        size_t readLength = file.read(data, CARD_SIZE);
        file.close();

        if (readLength != CARD_SIZE)
        {
            Serial.printf("LOCAL CARD: read failed %u/%u bytes\n",readLength,CARD_SIZE);
            return false;
        }

        memcpy(cardInfo, data, CARD_SIZE);

        cardHasData = true;
        insertCardHandle();

        Serial.printf("LOCAL CARD: loaded %s (%u bytes)\n",path.c_str(),CARD_SIZE);

        return true;
    }
    void handleSaveCurrentCard()
    {
        if (currentID.length() == 0)
        {
            server.send(204);
            return;
        }

        if (!cardHasData)
        {
            server.send(204);
            return;
        }

        if (!saveCardLocal())
        {
            server.send(500,"text/plain","Save failed");
            return;
        }

        server.send(200,"text/plain","SAVED");
    }
    bool uploadCardToDatabase()
    {
        if (!cardHasData)
        {
            Serial.println("CARD UPLOAD: no card data");
            return false;
        }

        if (currentID.length() == 0)
            currentID = "NOID";

        if (!cardLocalSavedForPendingUpload)
        {
            if (!saveCardLocal())
                return false;
            cardLocalSavedForPendingUpload = true;
        }

        if (!databaseEnabled)
        {
            Serial.println("LOCAL CARD: saved, external DB disabled");
            return true;
        }

        if (db.length() == 0)
        {
            Serial.println("CARD UPLOAD: DB not configured");
            return false;
        }

        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("CARD UPLOAD: Wi-Fi disconnected");
            return false;
        }

        String cardData =cardDataToHex();

        String url ="http://" +db +"/cards?id=" +urlEncode(currentID) +"&name=" +urlEncode(currentID) +"&carddata=" +cardData;

        Serial.println(currentID);
        Serial.println(url);

        HTTPClient http;

        http.setTimeout(5000);

        if (!http.begin(url))
        {
            return false;
        }

    int status =
        http.GET();

    String response;

    if (status > 0)
    {
        response =
            http.getString();
    }

    http.end();

    Serial.printf(
        "CARD UPLOAD HTTP STATUS: %d\n",
        status
    );

    if (response.length() > 0)
    {
        Serial.print("CARD UPLOAD RESPONSE: ");

        Serial.println(response);
    }
        if (status >= 200 && status < 300)
        {
            Serial.printf(
                "CARD UPLOAD OK: HTTP %d\n",
                status
            );

            if (response.length() > 0)
            {
                Serial.print(
                    "DB: ");

                Serial.println(
                    response);
            }
            currentID = "NOID";
            return true;
        }

        Serial.printf(
            "CARD UPLOAD FAILED: HTTP %d\n",
            status
        );

        return false;
    }

    void handleStatus()
    {
        String json;
        json.reserve(512);

        json += "{";

        json += "\"wifi\":";
        json += (WiFi.status() == WL_CONNECTED ? "true" : "false");

        json += ",\"ip\":\"";
        json += WiFi.localIP().toString();
        json += "\"";

        json += ",\"rssi\":";
        json += String(WiFi.RSSI());

        json += ",\"initialized\":";
        json += CardReaderIsInitialized() ? "true" : "false";

        json += ",\"fontsLoaded\":";
        json += String(CardReaderFontsLoaded());

        json += ",\"fontsComplete\":";
        json += CardReaderFontsComplete() ? "true" : "false";

        json += ",\"readerState\":\"";
        json += CardReaderGetStateText();
        json += "\"";

        json += ",\"cardHasData\":";
        json += cardHasData ? "true" : "false";

        json += ",\"cardInserted\":";
        json += cardInserted ? "true" : "false";

        json += "}";

        server.send(
            200,
            "application/json",
            json
        );
    }

    const char* wifiStatusText(wl_status_t status)
    {
        switch (status)
        {
            case WL_IDLE_STATUS:
                return "IDLE";

            case WL_NO_SSID_AVAIL:
                return "SSID NOT FOUND";

            case WL_SCAN_COMPLETED:
                return "SCAN COMPLETED";

            case WL_CONNECTED:
                return "CONNECTED";

            case WL_CONNECT_FAILED:
                return "CONNECT FAILED";

            case WL_CONNECTION_LOST:
                return "CONNECTION LOST";

            case WL_DISCONNECTED:
                return "DISCONNECTED";

            default:
                return "UNKNOWN";
        }
    }

    void connectWiFi()
    {
        WiFi.mode(WIFI_STA);
        // Serial.print("ESP32 Wi-Fi MAC: ");
        // Serial.println(WiFi.macAddress());

        WiFi.begin(WIFI_SSID);
        unsigned long lastStatusPrint = 0;

        while (WiFi.status() != WL_CONNECTED)
        {
            delay(100);

            if (millis() - lastStatusPrint >= 5000)
            {
                lastStatusPrint = millis();

                Serial.println();
                Serial.println("--- Wi-Fi not connected ---");

                Serial.print("MAC Address: ");
                Serial.println(WiFi.macAddress());

                Serial.print("Wi-Fi status: ");
                Serial.print(WiFi.status());
                Serial.print(" (");
                Serial.print(wifiStatusText(WiFi.status()));
                Serial.println(")");

                Serial.print("Target SSID: ");
                Serial.println(WIFI_SSID);

                Serial.println("---------------------------");
            }
        }
        //
        // Serial.print("MAC Address: ");
        // Serial.println(WiFi.macAddress());
        //
        // Serial.print("SSID: ");
        // Serial.println(WiFi.SSID());
        //
        // Serial.print("IP address: ");
        // Serial.println(WiFi.localIP());
        //
        // Serial.print("Gateway: ");
        // Serial.println(WiFi.gatewayIP());
        //
        // Serial.print("Subnet: ");
        // Serial.println(WiFi.subnetMask());
        //
        // Serial.print("Signal strength: ");
        // Serial.print(WiFi.RSSI());
        // Serial.println(" dBm");
        //
        // Serial.println();
    }

    void scanWiFi()
    {
        Serial.println();
        Serial.println("=================================");
        Serial.println("Scanning Wi-Fi networks...");
        Serial.println("=================================");

        int count = WiFi.scanNetworks();

        if (count == 0)
        {
            Serial.println("No Wi-Fi networks found.");
            return;
        }

        Serial.print("Found ");
        Serial.print(count);
        Serial.println(" networks:");

        for (int i = 0; i < count; i++)
        {
            Serial.print(i + 1);
            Serial.print(": ");

            Serial.print(WiFi.SSID(i));

            Serial.print(" | RSSI: ");
            Serial.print(WiFi.RSSI(i));
            Serial.print(" dBm");

            Serial.print(" | Channel: ");
            Serial.print(WiFi.channel(i));

            Serial.print(" | ");

            if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
            {
                Serial.print("OPEN");
            }
            else
            {
                Serial.print("SECURED");
            }

            Serial.println();
        }

        Serial.println();
    }

    void webInsertCard()
    {
        String result = loadCardForCurrentID();

        if (result == "DB_NOT_CONFIGURED")
        {
            server.send(400, "text/plain", "DB not configured");
            return;
        }

        if (result == "ID_NOT_CONFIGURED")
        {
            server.send(400, "text/plain", "ID not configured");
            return;
        }

        if (result == "NO_CARD")
        {
            server.send(200, "text/plain", "NO_CARD");
            return;
        }

        if (result == "CARD_INSERTED")
        {
            server.send(200, "text/plain", "CARD_INSERTED");
            return;
        }

        server.send(500, "text/plain", result);
    }

    void webRemoveCard()
    {
        removeCardHandle();

        server.send(200,"text/plain","CARD_REMOVED");
    }

    void webSerialSend()
    {
        if (!server.hasArg("hex"))
        {
            server.send(400,"text/plain","Missing hex parameter");

            return;
        }

        uint8_t data[256];
        size_t length = 0;

        String hex =
            server.arg("hex");

        if (!parseHex(hex,data,sizeof(data),length))
        {
            server.send(400,"text/plain","Invalid hex");

            return;
        }

        cardReaderSendRaw(
            data,
            length
        );

        String result = "Sent: ";

        for (size_t i = 0; i < length; i++)
        {
            char buffer[4];

            snprintf(
                buffer,
                sizeof(buffer),
                "%02X ",
                data[i]
            );

            result += buffer;
        }

        server.send(200,"text/plain",result);
    }

    void webSerialLoopback()
    {
        debug_CardReaderSendLoopbackTest();

        server.send(200,"text/plain","Sent: 55 AA 12 34 02 07 10");
    }

    void getConfig()
    {
        String json = "{";

        json += "\"db\":\"";
        json += db;
        json += "\",";

        json += "\"id\":\"";
        json += currentID;
        json += "\"";

        json += "}";

        server.send(
            200,
            "application/json",
            json
        );
    }

    void setConfig()
    {
        if (server.hasArg("db"))
        {
            db = server.arg("db");
            db.trim();
        }

        if (server.hasArg("id"))
        {
            currentID = server.arg("id");
            currentID.trim();
        }

        Serial.print("DB: ");
        Serial.println(db);

        Serial.print("Current ID: ");
        Serial.println(currentID);

        server.send(
            200,
            "text/plain",
            "OK"
        );
    }
}

void queueCardUpload()
{
    cardUploadPending = true;
    cardLocalSavedForPendingUpload = false;
    cardUploadQueuedAt = millis();

    Serial.println("CARD: eject complete, save queued");
}

void webInit()
{
    WiFi.mode(WIFI_STA);

    Serial.print("ESP32 Wi-Fi MAC: ");
    Serial.println(WiFi.macAddress());

    //scanWiFi();
    connectWiFi();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);

    server.on(
        "/api/card/insert",
        HTTP_GET,
        webInsertCard
    );

    server.on(
        "/api/card/remove",
        HTTP_GET,
        webRemoveCard
    );

    server.on(
        "/api/serial/send",
        HTTP_GET,
        webSerialSend
    );

    server.on(
        "/api/serial/loopback",
        HTTP_GET,
        webSerialLoopback
    );

    server.on(
        "/api/config",
        HTTP_GET,
        getConfig
    );

    server.on(
        "/api/config/set",
        HTTP_GET,
        setConfig
    );
    server.on(
        "/api/local/card/savecurrent",
        HTTP_POST,
        handleSaveCurrentCard
    );
    server.on(
        "/api/local/cards",
        HTTP_GET,
        handleLocalCards
    );

    server.on(
        "/api/local/card/delete",
        HTTP_POST,
        handleDeleteLocalCard
    );
    server.begin();

    Serial.println("HTTP server started");
    Serial.print("Open: http://");
    Serial.println(WiFi.localIP());
}

void webHandle()
{
    server.handleClient();

    if (!cardUploadPending)
        return;

    // Give Initial D time to completely finish
    // its card-eject communication before starting
    // a blocking HTTP request.
    if (
        millis() - cardUploadQueuedAt
        < 1000
    )
    {
        return;
    }

    // Retry failed uploads every 2 seconds.
    if (
        lastCardUploadAttempt != 0 &&
        millis() - lastCardUploadAttempt < 2000
    )
    {
        return;
    }

    lastCardUploadAttempt =
        millis();

    if (uploadCardToDatabase())
    {
        cardUploadPending = false;
    }
}

void setCurrentID(const String& id) {
    currentID = id;
}

String loadCardForCurrentID()
{
    if (currentID.length() == 0)
        return "ID_NOT_CONFIGURED";

    if (loadCardLocal(currentID))
        return "CARD_INSERTED";

    if (!databaseEnabled)
    {
        clearCardData();

        Serial.println(
            "LOCAL CARD: not found, external DB disabled"
        );

        return "NO_CARD";
    }

    if (db.length() == 0)
        return "DB_NOT_CONFIGURED";

    String url = "http://" + db + "/cards?id=" + urlEncode(currentID);

    Serial.print("Fetching card: ");
    Serial.println(url);

    HTTPClient http;
    http.setTimeout(2000);

    if (!http.begin(url))
        return "DB_CONNECTION_FAILED";

    int status = http.GET();

    if (status != 200)
    {
        http.end();
        return "DB_HTTP_" + String(status);
    }

    String response = http.getString();
    http.end();

    String cardData = extractCardData(response);
    cardData.trim();

    if (cardData == "00")
    {
        clearCardData();

        Serial.printf("No card found for ID: %s\n", currentID.c_str());

        return "NO_CARD";
    }

    if (cardData.length() != CARD_SIZE * 2)
    {
        Serial.printf("Invalid card length: %u\n", cardData.length());
        return "INVALID_CARD_LENGTH";
    }

    uint8_t data[CARD_SIZE];
    size_t length = 0;

    if (!parseHex(cardData, data, sizeof(data), length))
        return "INVALID_CARD_HEX";

    if (length != CARD_SIZE)
        return "INVALID_CARD_SIZE";

    memcpy(cardInfo, data, CARD_SIZE);

    cardHasData = true;
    insertCardHandle();

    Serial.printf(
        "Loaded card '%s': %u bytes\n",
        currentID.c_str(),
        CARD_SIZE
    );

    return "CARD_INSERTED";
}
void setDatabaseEnabled(bool enabled)
{
    databaseEnabled = enabled;

    Serial.printf(
        "External database: %s\n",
        databaseEnabled ? "ENABLED" : "DISABLED"
    );
}