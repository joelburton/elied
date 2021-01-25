/** ELIE-D
  
  Websocket device based around the TTGO-TDISPLAY (ESP 32 Arduino).
  
  Copyright (C) 2021 Joel Burton <joel@joelburton.com>.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses
 */

#include <Arduino.h>

#include <Preferences.h>

// Install from https://github.com/Bodmer/TFT_ST7735
#include <TFT_eSPI.h>
#include <SPI.h>

#include <IPAddress.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include <ArduinoJson.h>

#include <WebSocketsClient.h>
#include <SocketIOclient.h>

#define ELIED_VERSION "1.0"

#define LCD_WIDTH 240
#define LCD_HEIGHT 135

// Hardware GPI port numbers
#define LED 27
#define GREEN_BTN 35
#define RED_BTN 0

Preferences prefs;

// Structure of application prefs --- this is stored in EEPROM
typedef struct
{
    char ssid[64];
    char password[64];
    char host[64];
    int port;
    bool useLED;
} prefs_t;

prefs_t appPrefs = {};

TFT_eSPI tft = TFT_eSPI();

WiFiMulti wiFiMulti;
SocketIOclient socketIO;

// Application globals
bool blink;
bool pendingAck;

/************************************* UTILITIES ****************************/

/** Print message with word wrapping and font chosen by what would fit. */

void printMsg(const char *message)
{
    Serial.printf("[Print] msg=%s\n", message);

    // make a mutable version
    char *msg = (char *)alloca(1000);
    strcpy(msg, message);

    // find biggest font size that should fit text
    if (tft.textWidth(msg) / 3.8 > LCD_WIDTH)
    {
        tft.setFreeFont(&FreeSans9pt7b);
        Serial.println("[Print] 9pt font");
    }
    else if (tft.textWidth(msg) / 2.5 > LCD_WIDTH)
    {
        tft.setFreeFont(&FreeSans12pt7b);
        Serial.println("[Print] 12pt font");
    }
    else
    {
        tft.setFreeFont(&FreeSansBold18pt7b);
        Serial.println("[Print] 18pt font");
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(TL_DATUM);
    tft.setCursor(0, tft.fontHeight());

    char *lineStart = msg;
    char *lastGood = msg;
    int16_t width = 0;
    bool eom = false;

    // general strategy:
    //   loop, keeping track of last-space seen
    //   when we've made a line that's too wide to fit on screen,
    //   print it up to the last-good space, and then start again
    while (true)
    {
        // keep track of are-we-at-end-of-message, since we'll need to
        // tinker with the string-ending \0 during this
        eom = *msg == '\0';

        if (*msg == ' ' || eom)
        {
            // hit space of EOM: measure how long the curr line would be
            *msg = '\0';
            width = tft.textWidth(lineStart);
            if (!eom)
                *msg = ' ';

            if (width < LCD_WIDTH && eom)
            {
                // last line & fits: print it and stop loop
                tft.println(lineStart);
                break;
            }
            else if (width < LCD_WIDTH)
            {
                // found space, keep track of it and keep looking
                lastGood = msg++;
            }
            else
            {
                // gone too far; print up to last good & restart from there
                if (lastGood == lineStart)
                {
                    // special case: haven't found a space; this line is one long word
                    tft.println(lineStart);
                    lineStart = lastGood = msg++;
                }
                else
                {
                    *lastGood = '\0';
                    tft.println(lineStart);
                    *lastGood = ' ';
                    msg = lineStart = ++lastGood;
                }
            }
        }
        else
        {
            // for non-space/EOM, just keep going
            msg++;
        }
    }
}

/********************************** PREFERENCES ******************************/

/** Load EEPROM preferences and assign to appPrefs. */

void getPrefs()
{
    prefs.begin("prefs");

    // If setting up new ELIED, convenient to set these first time
    // strcpy(appPrefs.ssid, "Joel office");
    // strcpy(appPrefs.password, "");
    // strcpy(appPrefs.host, "elied.wschat.joelburton.net");
    // appPrefs.port = 80;
    // appPrefs.useLED = true;

    // Serial.printf("[First] ssid=%s\n", appPrefs.ssid);
    // Serial.printf("[First] password=%s\n", appPrefs.password);
    // Serial.printf("[First] host=%s\n", appPrefs.host);
    // Serial.printf("[First] port=%d\n", appPrefs.port);
    // Serial.printf("[First] useLED=%d\n", appPrefs.useLED);

    // prefs.putBytes("prefs", &appPrefs, sizeof(appPrefs));

    prefs.getBytes("prefs", &appPrefs, sizeof(appPrefs));

    Serial.printf("[Prefs] ssid=%s\n", appPrefs.ssid);
    Serial.printf("[Prefs] password=%s\n", appPrefs.password);
    Serial.printf("[Prefs] host=%s\n", appPrefs.host);
    Serial.printf("[Prefs] port=%d\n", appPrefs.port);
    Serial.printf("[Prefs] useLED=%d\n", appPrefs.useLED);
}

/***************** SPLASH SCREEN AND BOOT OPTIONS ****************************/

/** Show welcome and copyright message. */

void splashScreen()
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(TC_DATUM);
    tft.setFreeFont(&FreeSansBold24pt7b);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("ELIE-D", LCD_WIDTH / 2, 15);

    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("Copyright 2021 by FluffyLabs", LCD_WIDTH / 2, 62);
}

/** Show prompt for startup options and, if pressed, run selected. */

void startupOptions()
{
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(42, 95);
    tft.print("Options: ");
    tft.setTextColor(TFT_DARKGREEN);
    tft.print("(INFO) ");
    tft.setTextColor(TFT_RED);
    tft.print("(SETTINGS)");

    Serial.println("[Start] Waiting for startup option");
    delay(2000);

    // Hide startup choices
    tft.fillRect(0, 90, LCD_WIDTH, 20, TFT_BLACK);

    if (digitalRead(RED_BTN) == LOW)
        configMode();
    if (digitalRead(GREEN_BTN) == LOW)
        gpl();
}

/****************************** BOOT UP OPTIONS ******************************/

void gpl()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(0, 0, 1);

    tft.println("This program is free software: you can");
    tft.println("redistribute it and/or modify it under");
    tft.println("the terms of the GNU General Public");
    tft.println("License as published by the Free");
    tft.println("Software Foundation, either version 3");
    tft.println("of the License, or (at your option) any");
    tft.println("later version.\n");

    tft.println("This program is distributed in the hope");
    tft.println("that it will be useful, but WITHOUT");
    tft.println("ANY WARRANTY; without even the implied");
    tft.println("warranty of MERCHANTABILITY or FITNESS");
    tft.println("FOR A PARTICULAR PURPOSE.\n");

    tft.println("See the GNU General Public");
    tft.println("License for more details.");

    tft.setTextColor(TFT_DARKGREEN);
    tft.setTextDatum(BR_DATUM);
    tft.drawString("(OK)", LCD_WIDTH - 1, LCD_HEIGHT - 1);

    // In 2.5 secs, treat green button as restart
    delay(2500);
    Serial.println("[GPL  ] Waiting for button to restart");
    while (digitalRead(GREEN_BTN) == HIGH)
        ;
    ESP.restart();
}

/** Enter config mode (RED button held during startup) */

void configMode()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(0, 0, 2);

    tft.println("Config mode:\n");
    tft.println("python3 ");
    tft.println(" -m serial.tools.miniterm");
    tft.println(" /dev/tty.usbserial-*");
    tft.println(" 115200 -e --eol LF");

    // Will wait for 30 seconds for each prompt
    Serial.setTimeout(30000);
    Serial.print("\n\nPress ENTER to begin > ");
    Serial.readStringUntil('\n');

    getConfigViaSerial();

    prefs.putBytes("prefs", &appPrefs, sizeof(appPrefs));

    // Test retrieval and print the new settinggs
    getPrefs();

    Serial.println("\n\n[Setup] Restarting. Quit miniterm with CTRL-].\n\n");
    ESP.restart();
}

/** Prompt in serial for new SSID/Password, and set selections to global vars. */

void getConfigViaSerial()
{
    char newSsid[64];
    char newPassword[64];
    char newHost[64];
    int newPort;
    bool newLED;

    while (true)
    {
        Serial.println();

        Serial.printf(" ssid [%s] > ", appPrefs.ssid);
        strcpy(newSsid, Serial.readStringUntil('\n').c_str());
        if (newSsid[0] == '\0')
            strcpy(newSsid, appPrefs.ssid);

        Serial.printf(" password [%s] > ", appPrefs.password);
        strcpy(newPassword, Serial.readStringUntil('\n').c_str());
        if (newPassword[0] == '\0')
            strcpy(newPassword, appPrefs.password);

        Serial.printf(" host [%s] > ", appPrefs.host);
        strcpy(newHost, Serial.readStringUntil('\n').c_str());
        if (newHost[0] == '\0')
            strcpy(newHost, appPrefs.host);

        Serial.printf(" port [%d] > ", appPrefs.port);
        newPort = Serial.readStringUntil('\n').toInt();
        if (newPort == 0)
            newPort = appPrefs.port;

        while (true)
        {
            Serial.printf(" use LED? (y/n) [%s] > ", appPrefs.useLED ? "y" : "n");
            String newLEDInput = Serial.readStringUntil('\n');
            if (newLEDInput == '\0')
            {
                newLED = appPrefs.useLED;
                break;
            }
            else if (newLEDInput == "y" || newLEDInput == "Y")
            {
                newLED = true;
                break;
            }
            else if (newLEDInput == "n" || newLEDInput == "N")
            {
                newLED = false;
                break;
            }
        }

        Serial.printf("\n ssid     : %s", newSsid);
        Serial.printf("\n password : %s", newPassword);
        Serial.printf("\n host     : %s", newHost);
        Serial.printf("\n port     : %d", newPort);
        Serial.printf("\n use LED  : %s\n\n", newLED ? "yes" : "no");

        if (confirmYN(" ok? "))
        {
            strcpy(appPrefs.ssid, newSsid);
            strcpy(appPrefs.password, newPassword);
            strcpy(appPrefs.host, newHost);
            appPrefs.port = newPort;
            appPrefs.useLED = newLED;
            return;
        }
    }
}

/** Prompt serial for y/n response; return t/f. */

bool confirmYN(const char *prompt)
{
    while (true)
    {
        Serial.printf("%s(y/n) > ", prompt);
        String ok = Serial.readStringUntil('\n');
        if (ok == "n" || ok == "N")
            return false;
        if (ok == "y" || ok == "Y")
            return true;
    }
}

/******************************** WIFI & WEB SOCKET **************************/

void connectToWifi()
{
    Serial.printf("[Wifi ] Trying to connect to %s", appPrefs.ssid);
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(appPrefs.ssid, 0, 95, 2);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_PINK);
    tft.drawString("***", LCD_WIDTH - 1, 95, 2);

    // disable AP
    if (WiFi.getMode() & WIFI_AP)
        WiFi.softAPdisconnect(true);

    wiFiMulti.addAP(appPrefs.ssid, appPrefs.password);

    while (int status = wiFiMulti.run() != WL_CONNECTED)
    {
        tft.setTextColor(TFT_YELLOW);
        tft.drawString("***", LCD_WIDTH - 1, 95, 2);
        Serial.printf(" %d", status);
        delay(100);
        tft.setTextColor(TFT_PINK);
        tft.drawString("***", LCD_WIDTH - 1, 95, 2);
        delay(100);
        if (digitalRead(RED_BTN) == LOW)
            esp_deep_sleep_start();
    }

    // Remove asterisks
    tft.setTextColor(TFT_BLACK);
    tft.drawString("***", LCD_WIDTH - 1, 95, 2);

    String ip = WiFi.localIP().toString();
    Serial.printf("\n[Wifi ] Connected to %s\n", ip.c_str());
    tft.setTextColor(TFT_DARKGREEN);
    tft.drawString(ip, LCD_WIDTH - 1, 95, 2);
}

void setupSocketIO()
{
    // Show connection attempt
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(appPrefs.host, 0, 115, 2);

    socketIO.begin(appPrefs.host, appPrefs.port);

    // Register SocketIO listener --- this handles connection & msgs
    socketIO.onEvent(socketIOEvent);
}

/** Handle Socket IO events */

void socketIOEvent(socketIOmessageType_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case sIOtype_DISCONNECT:
        // Show message; device will automatically try to reconnect
        Serial.printf("[WSock] Disconnected!\n");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Socket disconnected", LCD_WIDTH / 2, LCD_HEIGHT / 2 - 5);
        blink = false;
        break;
    case sIOtype_CONNECT:
        // Show connection message
        Serial.printf("[WSock] Connected to url: %s\n", payload);
        socketIO.send(sIOtype_CONNECT, "/");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(TFT_DARKGREEN, TFT_BLACK);
        tft.drawString("Socket established", LCD_WIDTH / 2, LCD_HEIGHT / 2 - 5);
        break;
    case sIOtype_EVENT:
        // Get message & show it
        Serial.printf("[WSock] get event: %s\n", payload);
        handleMessage((char *)payload);
        break;
    case sIOtype_ACK:
        Serial.printf("[WSock] get ack: %u\n", length);
        break;
    case sIOtype_ERROR:
        Serial.printf("[WSock] get error: %u\n", length);
        break;
    case sIOtype_BINARY_EVENT:
        Serial.printf("[WSock] get binary: %u\n", length);
        break;
    case sIOtype_BINARY_ACK:
        Serial.printf("[WSock] get binary ack: %u\n", length);
        break;
    }
}

/** Handle incoming message */

void handleMessage(char *payload)
{
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    const char *type = doc[0];
    const char *text = doc[1];
    if (strcmp(type, "msg") == 0)
    {
        Serial.printf("[Chat ] message=%s\n", text);
        printMsg(text);
        blink = appPrefs.useLED;
    }
}

/** Send ACK of message (called on message clear). */

void sendAck()
{
    DynamicJsonDocument doc(1024);
    JsonArray array = doc.to<JsonArray>();

    array.add("ack");
    array.add("ELIE-D received");

    String output;
    serializeJson(doc, output);

    Serial.printf("[Ack  ] message=%s\n", output.c_str());
    socketIO.sendEVENT(output);
}

/***************** MAIN ARDUINO FUNCTIONS ************************************/

/** Start of code:
 * 
 * - Setup serial output
 * - Set hardware input/output modes
 * - Read preferences from EEPROM
 * - Setup screen
 * - Show splash screen
 * - Show boot options & wait to see if pressed
 * - Connect to router
 * - Set up web socket
 * - Attach interrupts for button presses
 * 
 * After this runs, Arduino keeps calling the loop() function.
 */

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(250);
    Serial.printf("\n\n[ElieD] Booting version %s\n", ELIED_VERSION);

    pinMode(LED, OUTPUT);
    pinMode(GREEN_BTN, INPUT);
    pinMode(RED_BTN, INPUT);

    getPrefs();

    tft.init();
    tft.setRotation(1);
    splashScreen();
    startupOptions();

    connectToWifi();
    setupSocketIO();

    attachInterrupt(digitalPinToInterrupt(GREEN_BTN), clearMessage, FALLING);
    attachInterrupt(digitalPinToInterrupt(RED_BTN), turnOff, FALLING);
}

/** When red button is pushed, put unit in deep sleep ("off") */

void turnOff()
{
    esp_deep_sleep_start();
}

/** When green button pushed, acknowledge message and clear */

void clearMessage()
{
    // If we're already in a clearmessage state, don't do again
    if (!pendingAck)
    {
        blink = false;
        pendingAck = true;
    }
}

/** Main loop:
 * 
 * Blink light if unconfirmed message shown.
 * 
 * Is interrupted by socket/button events.
 */

void loop()
{
    socketIO.loop();

    if (blink)
        digitalWrite(LED, HIGH);
    delay(10);

    digitalWrite(LED, LOW);
    delay(350);

    if (pendingAck)
    {
        sendAck();
        // It's not safe to do this work inside of the interrupt function itself,
        // so this is deferred to here
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("Waiting for message", LCD_WIDTH / 2, LCD_HEIGHT / 2 - 5);
        // Debounce button
        delay(500);
        pendingAck = false;
    }
}
