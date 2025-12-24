#include <FastLED.h>

#define DATA_PIN 14
#define NUM_LEDS 62
#define RECEIVED_LEDS                                                                              \
        ((CAPTURE_HEIGHT - CAPTURE_DEPTH) / CAPTURE_DEPTH + 1 +                                    \
         (CAPTURE_WIDTH + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH +                                     \
         (CAPTURE_HEIGHT + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH)
#define CAPTURE_WIDTH 256
#define CAPTURE_HEIGHT 144
#define CAPTURE_DEPTH 10
#define SERIAL_BAUD 921600
#define BUFFER_SIZE (RECEIVED_LEDS * 3)
#define TIMEOUT_MS 10000

CRGB leds[NUM_LEDS];
CRGB targetColors[NUM_LEDS];
uint8_t rxBuffer[BUFFER_SIZE];

uint8_t g_brightness = 150;
uint8_t g_saturation_boost = 1;
uint8_t g_smoothing = 100;

unsigned long lastFrameTime = 0;
bool ledsActive = true;

void waitForConfig() {
        uint8_t configBuffer[4];

        while (true) {
                if (Serial.available() >= 4) {
                        Serial.readBytes(configBuffer, 4);

                        if (configBuffer[0] == 0xFF && configBuffer[1] == 0xAA) {
                                g_brightness = configBuffer[2];
                                g_saturation_boost = configBuffer[3];

                                FastLED.setBrightness(g_brightness);
                                fill_solid(leds, NUM_LEDS, CRGB::Green);
                                FastLED.show();
                                delay(500);
                                FastLED.clear();
                                FastLED.show();

                                return;
                        }
                }
                delay(10);
        }
}

void setup() {
        Serial.begin(SERIAL_BAUD);
        Serial.setTimeout(100);

        delay(100);
        while (Serial.available())
                Serial.read();

        FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);

        FastLED.setBrightness(50);
        fill_solid(leds, NUM_LEDS, CRGB::Blue);
        FastLED.show();

        waitForConfig();

        lastFrameTime = millis();
}

void loop() {
        if (millis() - lastFrameTime > TIMEOUT_MS) {
                if (ledsActive) {
                        FastLED.clear();
                        FastLED.show();
                        ledsActive = false;
                }
        }

        if (Serial.available() >= BUFFER_SIZE) {
                size_t bytesRead = Serial.readBytes(rxBuffer, BUFFER_SIZE);

                if (bytesRead == BUFFER_SIZE) {
                        if (rxBuffer[0] == 0xFF && rxBuffer[1] == 0xAA) {
                                g_brightness = rxBuffer[2];
                                g_saturation_boost = rxBuffer[3];
                                FastLED.setBrightness(g_brightness);

                                while (Serial.available())
                                        Serial.read();
                                return;
                        }

                        for (int i = 0; i < NUM_LEDS; i++) {
                                int srcIndex = (i * RECEIVED_LEDS) / NUM_LEDS;
                                srcIndex = min(srcIndex, RECEIVED_LEDS - 1);

                                int offset = srcIndex * 3;

                                CRGB color = CRGB(rxBuffer[offset + 0], rxBuffer[offset + 1],
                                                  rxBuffer[offset + 2]);

                                CHSV hsv = rgb2hsv_approximate(color);

                                if (g_saturation_boost > 0) {
                                        uint16_t sat_range = 255 - hsv.s;
                                        uint16_t sat_increase =
                                            (sat_range * g_saturation_boost) / 255;
                                        hsv.s = hsv.s + sat_increase;
                                }

                                targetColors[i] = hsv;
                        }

                        for (int i = 0; i < NUM_LEDS; i++) {
                                leds[i] = blend(leds[i], targetColors[i], g_smoothing);
                        }

                        FastLED.show();
                        lastFrameTime = millis();
                        ledsActive = true;
                }

                while (Serial.available())
                        Serial.read();
        }
}
