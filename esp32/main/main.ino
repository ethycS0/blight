#include <FastLED.h>

#define WIFI
// #define SERIAL

#ifdef WIFI
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#endif

#define DATA_PIN 14
#define NUM_LEDS 62
#define CAPTURE_WIDTH 160
#define CAPTURE_HEIGHT 90
#define CAPTURE_DEPTH 10
#define RECEIVED_LEDS                                                                              \
        ((CAPTURE_HEIGHT - CAPTURE_DEPTH) / CAPTURE_DEPTH + 1 +                                    \
         (CAPTURE_WIDTH + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH +                                     \
         (CAPTURE_HEIGHT + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH)

#ifdef SERIAL
#define SERIAL_BAUD 921600
#endif

#ifdef WIFI

#if __has_include("creds.h")
#include "creds.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YourSSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YourPassword"
#endif

const uint16_t UDP_PORT = 4210;
IPAddress staticIP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 1, 1);
IPAddress secondaryDNS(8, 8, 8, 8);
WiFiUDP udp;
#endif

#define BUFFER_SIZE (RECEIVED_LEDS * 3)
#define TIMEOUT_MS 10000
#define STARTUP_BLINK_DURATION 2000

enum SystemState { STATE_WAITING_CONFIG, STATE_ACTIVE, STATE_TIMEOUT };

CRGB leds[NUM_LEDS];
uint8_t rxBuffer[BUFFER_SIZE];

uint8_t g_brightness = 150;

unsigned long lastFrameTime = 0;
SystemState currentState = STATE_WAITING_CONFIG;

void startupBlink() {
        unsigned long startTime = millis();
        bool ledState = true;

        while (millis() - startTime < STARTUP_BLINK_DURATION) {
                if (ledState) {
                        fill_solid(leds, NUM_LEDS, CRGB(73, 16, 230));
                } else {
                        FastLED.clear();
                }
                FastLED.show();
                ledState = !ledState;
                delay(200);
        }

        FastLED.clear();
        FastLED.show();
}

bool processConfigPacket(uint8_t *buffer, size_t size) {
        if (size == 4 && buffer[0] == 0xFF && buffer[1] == 0xAA) {
                g_brightness = buffer[2];
                FastLED.setBrightness(g_brightness);

                fill_solid(leds, NUM_LEDS, CRGB::Green);
                FastLED.show();
                delay(100);
                FastLED.clear();
                FastLED.show();

                return true;
        }
        return false;
}

void waitForConfig() {
        uint8_t configBuffer[4];

        currentState = STATE_WAITING_CONFIG;

        while (true) {
                bool hasData = false;
                size_t bytesRead = 0;

#ifdef SERIAL
                if (Serial.available() >= 4) {
                        bytesRead = Serial.readBytes(configBuffer, 4);
                        hasData = (bytesRead == 4);
                }
#endif

#ifdef WIFI
                int packetSize = udp.parsePacket();
                if (packetSize >= 4) {
                        bytesRead = udp.read(configBuffer, 4);
                        hasData = (bytesRead >= 4);
                }
#endif

                if (hasData) {
                        if (processConfigPacket(configBuffer, bytesRead)) {
                                currentState = STATE_ACTIVE;
                                lastFrameTime = millis();
                                return;
                        }
                }

                yield();
                delay(10);
        }
}

void enterTimeoutState() {
        if (currentState != STATE_TIMEOUT) {
                FastLED.clear();
                FastLED.show();
                currentState = STATE_TIMEOUT;

#ifdef SERIAL
                while (Serial.available())
                        Serial.read();
#endif

                waitForConfig();
        }
}

#ifdef WIFI
void optimizeWiFi() {
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
        esp_wifi_set_max_tx_power(84);
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        conf.sta.listen_interval = 1;
        esp_wifi_set_config(WIFI_IF_STA, &conf);
        Serial.println("WiFi optimized for low latency");
}
#endif

void setup() {
#ifdef SERIAL
        Serial.begin(SERIAL_BAUD);
        Serial.setTimeout(100);
        delay(100);
        while (Serial.available())
                Serial.read();
#endif

#ifdef WIFI
        Serial.begin(115200);

        WiFi.setSleep(false);

        if (!WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS)) {
                Serial.println("Static IP configuration failed!");
        }

        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);

        Serial.print("Connecting to WiFi");
        unsigned long wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
                delay(500);
                Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\nWiFi connected");
                Serial.print("IP: ");
                Serial.println(WiFi.localIP());

                optimizeWiFi();
        } else {
                Serial.println("\nWiFi connection failed!");
        }

        udp.begin(UDP_PORT);

        Serial.printf("UDP listening on port %d\n", UDP_PORT);
#endif

        FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(50);

        startupBlink();

        waitForConfig();
}

void loop() {
        if (currentState == STATE_ACTIVE && millis() - lastFrameTime > TIMEOUT_MS) {
                enterTimeoutState();
                return;
        }

        bool dataAvailable = false;
        size_t bytesRead = 0;

#ifdef WIFI
        int packetSize = udp.parsePacket();
        if (packetSize >= BUFFER_SIZE) {
                bytesRead = udp.read(rxBuffer, BUFFER_SIZE);
                dataAvailable = (bytesRead == BUFFER_SIZE);
        } else if (packetSize >= 4) {
                uint8_t tempBuf[4];
                bytesRead = udp.read(tempBuf, 4);
                if (processConfigPacket(tempBuf, bytesRead)) {
                        lastFrameTime = millis();
                        if (currentState != STATE_ACTIVE) {
                                currentState = STATE_ACTIVE;
                        }
                        return;
                }
        } else if (packetSize > 0) {
                udp.flush();
        }
#endif

#ifdef SERIAL
        if (Serial.available() >= BUFFER_SIZE) {
                bytesRead = Serial.readBytes(rxBuffer, BUFFER_SIZE);
                dataAvailable = (bytesRead == BUFFER_SIZE);

                if (!dataAvailable) {
                        while (Serial.available())
                                Serial.read();
                }
        }
#endif

        if (dataAvailable) {
                if (processConfigPacket(rxBuffer, bytesRead)) {
                        lastFrameTime = millis();
                        if (currentState != STATE_ACTIVE) {
                                currentState = STATE_ACTIVE;
                        }
#ifdef SERIAL
                        while (Serial.available())
                                Serial.read();
#endif
                        return;
                }

                for (int i = 0; i < NUM_LEDS; i++) {
                        int srcIndex = (i * RECEIVED_LEDS) / NUM_LEDS;
                        srcIndex = constrain(srcIndex, 0, RECEIVED_LEDS - 1);
                        int offset = srcIndex * 3;

                        leds[i] =
                            CRGB(rxBuffer[offset + 0], rxBuffer[offset + 1], rxBuffer[offset + 2]);
                }

                FastLED.show();
                lastFrameTime = millis();
                currentState = STATE_ACTIVE;

#ifdef SERIAL
                while (Serial.available())
                        Serial.read();
#endif
        }

        yield();
}
