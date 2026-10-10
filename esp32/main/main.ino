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
float g_saturation = 1.0f;
float g_smoothing = 1.0f;

struct ColorFloat {
        float r, g, b;
};

ColorFloat smoothed_colors[NUM_LEDS];
bool first_frame = true;

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

void boost_saturation_f(float &r, float &g, float &b, float boost) {
        if (boost <= 1.0f) return;

        float max_val = max(r, max(g, b));
        float min_val = min(r, min(g, b));
        float delta = max_val - min_val;

        if (delta < 0.001f) return;

        float h, s, v = max_val;
        s = delta / max_val;

        if (r == max_val)
                h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
        else if (g == max_val)
                h = (b - r) / delta + 2.0f;
        else
                h = (r - g) / delta + 4.0f;
        h /= 6.0f;

        s = min(s * boost, 1.0f);

        int i = (int)(h * 6.0f);
        float f = h * 6.0f - i;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        }
}

bool processConfigPacket(uint8_t *buffer, size_t size) {
        if (size >= 4 && buffer[0] == 0xFF && buffer[1] == 0xAA) {
                g_brightness = buffer[2];
                FastLED.setBrightness(g_brightness);
                
                if (size >= 12) {
                        float temp_sat, temp_smooth;
                        memcpy(&temp_sat, &buffer[4], sizeof(float));
                        memcpy(&temp_smooth, &buffer[8], sizeof(float));
                        if (!isnan(temp_sat) && isfinite(temp_sat) && temp_sat >= 0.0f && temp_sat <= 10.0f) {
                                g_saturation = temp_sat;
                        }
                        if (!isnan(temp_smooth) && isfinite(temp_smooth) && temp_smooth >= 0.01f && temp_smooth <= 1.0f) {
                                g_smoothing = temp_smooth;
                        }
                }

                if (currentState != STATE_ACTIVE) {
                        fill_solid(leds, NUM_LEDS, CRGB::Green);
                        FastLED.show();
                        delay(100);
                        FastLED.clear();
                        FastLED.show();
                }

                return true;
        }
        return false;
}

void processFramePacket(const uint8_t *buffer, size_t size) {
        if (size != BUFFER_SIZE) return;

        for (int i = 0; i < NUM_LEDS; i++) {
                int srcIndex = (i * RECEIVED_LEDS) / NUM_LEDS;
                srcIndex = constrain(srcIndex, 0, RECEIVED_LEDS - 1);
                int offset = srcIndex * 3;

                float target_r = buffer[offset + 0] / 255.0f;
                float target_g = buffer[offset + 1] / 255.0f;
                float target_b = buffer[offset + 2] / 255.0f;

                boost_saturation_f(target_r, target_g, target_b, g_saturation);

                if (first_frame) {
                        smoothed_colors[i].r = target_r;
                        smoothed_colors[i].g = target_g;
                        smoothed_colors[i].b = target_b;
                } else {
                        smoothed_colors[i].r = g_smoothing * target_r + (1.0f - g_smoothing) * smoothed_colors[i].r;
                        smoothed_colors[i].g = g_smoothing * target_g + (1.0f - g_smoothing) * smoothed_colors[i].g;
                        smoothed_colors[i].b = g_smoothing * target_b + (1.0f - g_smoothing) * smoothed_colors[i].b;
                }

                leds[i] = CRGB(
                    (uint8_t)(constrain(smoothed_colors[i].r * 255.0f + 0.5f, 0.0f, 255.0f)),
                    (uint8_t)(constrain(smoothed_colors[i].g * 255.0f + 0.5f, 0.0f, 255.0f)),
                    (uint8_t)(constrain(smoothed_colors[i].b * 255.0f + 0.5f, 0.0f, 255.0f))
                );
        }
        first_frame = false;

        FastLED.show();
}

void enterTimeoutState() {
        if (currentState != STATE_TIMEOUT) {
                FastLED.clear();
                FastLED.show();
                currentState = STATE_TIMEOUT;
                first_frame = true;

#ifdef SERIAL
                while (Serial.available())
                        Serial.read();
#endif
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

void checkWiFiConnection() {
        static bool wasConnected = false;
        static unsigned long lastWifiRetry = 0;

        if (WiFi.status() == WL_CONNECTED) {
                if (!wasConnected) {
                        Serial.println("\nWiFi connected");
                        Serial.print("IP: ");
                        Serial.println(WiFi.localIP());

                        optimizeWiFi();
                        udp.stop();
                        udp.begin(UDP_PORT);
                        wasConnected = true;
                }
        } else {
                wasConnected = false;
                if (millis() - lastWifiRetry > 5000) {
                        lastWifiRetry = millis();
                        Serial.println("\nWiFi disconnected. Attempting reconnect...");
                        WiFi.disconnect();
                        WiFi.reconnect();
                }
        }
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
        while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
                delay(500);
                Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\nWiFi connected");
                Serial.print("IP: ");
                Serial.println(WiFi.localIP());

                optimizeWiFi();
                udp.begin(UDP_PORT);
                Serial.printf("UDP listening on port %d\n", UDP_PORT);
        } else {
                Serial.println("\nWiFi connection failed! Will retry in background.");
        }
#endif

        FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(g_brightness);

        startupBlink();

        currentState = STATE_WAITING_CONFIG;
}

void loop() {
#ifdef WIFI
        checkWiFiConnection();
#endif

        if (currentState == STATE_ACTIVE && millis() - lastFrameTime > TIMEOUT_MS) {
                enterTimeoutState();
        }

#ifdef WIFI
        if (WiFi.status() == WL_CONNECTED) {
                int packetSize = udp.parsePacket();
                if (packetSize == BUFFER_SIZE) {
                        size_t bytesRead = udp.read(rxBuffer, BUFFER_SIZE);
                        if (bytesRead == BUFFER_SIZE) {
                                processFramePacket(rxBuffer, BUFFER_SIZE);
                                lastFrameTime = millis();
                                currentState = STATE_ACTIVE;
                        }
                } else if (packetSize >= 4 && packetSize < BUFFER_SIZE) {
                        uint8_t tempBuf[12];
                        size_t bytesRead = udp.read(tempBuf, sizeof(tempBuf));
                        if (bytesRead >= 4 && tempBuf[0] == 0xFF && tempBuf[1] == 0xAA) {
                                if (processConfigPacket(tempBuf, bytesRead)) {
                                        lastFrameTime = millis();
                                        currentState = STATE_ACTIVE;
                                }
                        }
                } else if (packetSize > 0) {
                        udp.flush();
                }
        }
#endif

#ifdef SERIAL
        if (Serial.available() >= BUFFER_SIZE) {
                size_t bytesRead = Serial.readBytes(rxBuffer, BUFFER_SIZE);
                if (bytesRead == BUFFER_SIZE) {
                        processFramePacket(rxBuffer, BUFFER_SIZE);
                        lastFrameTime = millis();
                        currentState = STATE_ACTIVE;
                }
                while (Serial.available())
                        Serial.read();
        } else if (Serial.available() >= 4) {
                uint8_t tempBuf[12];
                size_t bytesRead = Serial.readBytes(tempBuf, sizeof(tempBuf));
                if (bytesRead >= 4 && tempBuf[0] == 0xFF && tempBuf[1] == 0xAA) {
                        if (processConfigPacket(tempBuf, bytesRead)) {
                                lastFrameTime = millis();
                                currentState = STATE_ACTIVE;
                        }
                }
                while (Serial.available())
                        Serial.read();
        }
#endif

        yield();
}

