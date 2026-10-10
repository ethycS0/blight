#include <FastLED.h>

#define WIFI
// #define SERIAL

#ifdef WIFI
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
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

// Fixed light quality settings. Saturation handling was removed: the host now
// sends the physically-correct average color straight through, so the LEDs
// match the screen without an HSV boost that skewed hue/saturation.
#define LED_BRIGHTNESS 250
// Interpolation time constant. Frames arrive at the host's capture rate, but
// the LEDs are eased toward the latest target against real elapsed time so
// motion stays smooth regardless of packet jitter or rate.
#define LED_SMOOTHING_TAU_MS 40.0f
#define LED_ANIM_INTERVAL_MS 10 // ~100Hz animation

// Command packets share the 0xFF 0xAA magic prefix and must match src/main.c.
#define CMD_CONFIG 0x01 // [3] = brightness
#define CMD_STATIC 0x02 // [3..5] = R, G, B
#define CMD_LIVE 0x03   // no payload, resume screen-capture mode
#define CMD_FORCE_LIVE 0x04 // no payload, resume live and clear phone override

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
// If WiFi is up but no UDP packet has arrived for this long, rebind the socket.
// The socket can silently stop delivering packets while WiFi still answers
// ping, which used to leave the strip dead until a manual reset.
#define RX_WATCHDOG_MS 5000
#define COMMAND_BUFFER_SIZE 8
#define STARTUP_BLINK_DURATION 2000

enum SystemState { STATE_WAITING_CONFIG, STATE_ACTIVE, STATE_STATIC, STATE_TIMEOUT };

CRGB leds[NUM_LEDS];
uint8_t rxBuffer[BUFFER_SIZE];

uint8_t g_brightness = LED_BRIGHTNESS;

struct ColorFloat {
        float r, g, b;
};

ColorFloat target_colors[NUM_LEDS];
ColorFloat smoothed_colors[NUM_LEDS];
bool first_frame = true;

unsigned long lastFrameTime = 0;
unsigned long lastAnimationTime = 0;
unsigned long lastPacketTime = 0;
SystemState currentState = STATE_WAITING_CONFIG;

// When true, the host's live stream is ignored so the phone/web controls win
// until the user taps "Live (screen)". Lets the phone take over without having
// to stop blight live on the PC.
bool manualOverride = false;

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

// Eases the physical LEDs toward target_colors[] using a time-based factor and
// pushes them out. Shared by live and static modes so transitions feel identical.
void updateLEDs(float alpha) {
        for (int i = 0; i < NUM_LEDS; i++) {
                float tr = target_colors[i].r;
                float tg = target_colors[i].g;
                float tb = target_colors[i].b;

                if (first_frame) {
                        smoothed_colors[i].r = tr;
                        smoothed_colors[i].g = tg;
                        smoothed_colors[i].b = tb;
                } else {
                        smoothed_colors[i].r =
                            alpha * tr + (1.0f - alpha) * smoothed_colors[i].r;
                        smoothed_colors[i].g =
                            alpha * tg + (1.0f - alpha) * smoothed_colors[i].g;
                        smoothed_colors[i].b =
                            alpha * tb + (1.0f - alpha) * smoothed_colors[i].b;
                }

                leds[i] = CRGB(
                    (uint8_t)(constrain(smoothed_colors[i].r * 255.0f + 0.5f, 0.0f, 255.0f)),
                    (uint8_t)(constrain(smoothed_colors[i].g * 255.0f + 0.5f, 0.0f, 255.0f)),
                    (uint8_t)(constrain(smoothed_colors[i].b * 255.0f + 0.5f, 0.0f, 255.0f)));
        }
        first_frame = false;

        FastLED.show();
}

void setAllTargets(uint8_t r, uint8_t g, uint8_t b) {
        float fr = r / 255.0f;
        float fg = g / 255.0f;
        float fb = b / 255.0f;
        for (int i = 0; i < NUM_LEDS; i++) {
                target_colors[i].r = fr;
                target_colors[i].g = fg;
                target_colors[i].b = fb;
        }
}

// Returns true when the packet was a recognized command.
bool processCommandPacket(const uint8_t *buffer, size_t size) {
        if (size < 4 || buffer[0] != 0xFF || buffer[1] != 0xAA)
                return false;

        switch (buffer[2]) {
        case CMD_CONFIG:
                if (manualOverride) // phone/web controls the brightness
                        return true;
                g_brightness = buffer[3];
                FastLED.setBrightness(g_brightness);
                return true;

        case CMD_STATIC:
                if (size < 6)
                        return false;
                setAllTargets(buffer[3], buffer[4], buffer[5]);
                currentState = STATE_STATIC;
                manualOverride = false; // an explicit host static takes over
                return true;

        case CMD_LIVE:
                if (manualOverride) // let the phone keep control
                        return true;
                currentState = STATE_ACTIVE;
                lastFrameTime = millis();
                return true;

        case CMD_FORCE_LIVE:
                // Sent once when a new `blight live` session starts, so starting
                // the PC tool reclaims control from the phone.
                manualOverride = false;
                currentState = STATE_ACTIVE;
                lastFrameTime = millis();
                return true;
        }

        return false;
}

void processFramePacket(const uint8_t *buffer, size_t size) {
        if (size != BUFFER_SIZE)
                return;

        for (int i = 0; i < NUM_LEDS; i++) {
                int srcIndex = (i * RECEIVED_LEDS) / NUM_LEDS;
                srcIndex = constrain(srcIndex, 0, RECEIVED_LEDS - 1);
                int offset = srcIndex * 3;

                target_colors[i].r = buffer[offset + 0] / 255.0f;
                target_colors[i].g = buffer[offset + 1] / 255.0f;
                target_colors[i].b = buffer[offset + 2] / 255.0f;
        }
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
// ---------------------------------------------------------------------------
// Phone control: a tiny HTTP server so the strip can be driven from a browser
// on the same WiFi. Endpoints are intentionally trivial GETs so they can also
// be curled or bookmarked.
// ---------------------------------------------------------------------------
WebServer webServer(80);
bool webServerStarted = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Blight</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;background:#111;color:#eee;margin:0;padding:24px;text-align:center}
h1{font-size:1.3rem;font-weight:600;letter-spacing:.05em}
.row{margin:18px 0}
button{font-size:1rem;padding:12px 18px;margin:4px;border:0;border-radius:10px;background:#2a2a2a;color:#eee}
button:active{background:#444}
input[type=color]{width:88px;height:52px;border:0;background:none}
input[type=range]{width:80%;vertical-align:middle}
.sw{width:44px;height:44px;border-radius:10px;display:inline-block;margin:3px;border:1px solid #333;vertical-align:middle}
.lbl{display:block;opacity:.6;font-size:.8rem;margin-bottom:6px}
</style>
</head>
<body>
<h1>BLIGHT</h1>
<div class="row"><span class="lbl">Color</span><input type="color" id="c" value="#ff8800" onchange="setColor()"></div>
<div class="row">
<span class="sw" style="background:#ffffff" onclick="preset('#ffffff')"></span>
<span class="sw" style="background:#ff0000" onclick="preset('#ff0000')"></span>
<span class="sw" style="background:#ff8800" onclick="preset('#ff8800')"></span>
<span class="sw" style="background:#ffee00" onclick="preset('#ffee00')"></span>
<span class="sw" style="background:#00ff44" onclick="preset('#00ff44')"></span>
<span class="sw" style="background:#0088ff" onclick="preset('#0088ff')"></span>
<span class="sw" style="background:#8800ff" onclick="preset('#8800ff')"></span>
</div>
<div class="row"><span class="lbl">Brightness</span><input type="range" id="b" min="0" max="255" value="250" onchange="setBright()" oninput="setBright()"></div>
<div class="row">
<button onclick="go('/live')">Live (screen)</button>
<button onclick="go('/off')">Off</button>
</div>
<script>
function setColor(){var v=document.getElementById('c').value;
var r=parseInt(v.substr(1,2),16),g=parseInt(v.substr(3,2),16),b=parseInt(v.substr(5,2),16);
fetch('/set?r='+r+'&g='+g+'&b='+b);}
function preset(v){document.getElementById('c').value=v;setColor();}
function setBright(){fetch('/bright?b='+document.getElementById('b').value);}
function go(u){fetch(u);}
</script>
</body>
</html>
)HTML";

void handleRoot() { webServer.send_P(200, "text/html", INDEX_HTML); }

void handleSet() {
        if (!webServer.hasArg("r") || !webServer.hasArg("g") || !webServer.hasArg("b")) {
                webServer.send(400, "text/plain", "need r,g,b");
                return;
        }
        setAllTargets((uint8_t)webServer.arg("r").toInt(), (uint8_t)webServer.arg("g").toInt(),
                      (uint8_t)webServer.arg("b").toInt());
        currentState = STATE_STATIC;
        manualOverride = true;
        first_frame = true;
        webServer.send(200, "text/plain", "ok");
}

void handleBright() {
        if (!webServer.hasArg("b")) {
                webServer.send(400, "text/plain", "need b");
                return;
        }
        g_brightness = (uint8_t)constrain(webServer.arg("b").toInt(), 0, 255);
        FastLED.setBrightness(g_brightness);
        webServer.send(200, "text/plain", "ok");
}

void handleOff() {
        setAllTargets(0, 0, 0);
        currentState = STATE_STATIC;
        manualOverride = true;
        first_frame = true;
        webServer.send(200, "text/plain", "ok");
}

void handleLive() {
        manualOverride = false;
        currentState = STATE_ACTIVE;
        lastFrameTime = millis();
        webServer.send(200, "text/plain", "ok");
}

void handleStatus() {
        char buf[64];
        snprintf(buf, sizeof(buf), "state=%d brightness=%d override=%d", (int)currentState,
                 (int)g_brightness, (int)manualOverride);
        webServer.send(200, "text/plain", buf);
}

void startWebServer() {
        if (webServerStarted)
                return;
        webServer.on("/", handleRoot);
        webServer.on("/set", handleSet);
        webServer.on("/bright", handleBright);
        webServer.on("/off", handleOff);
        webServer.on("/live", handleLive);
        webServer.on("/status", handleStatus);
        webServer.begin();
        webServerStarted = true;
        Serial.println("HTTP control on port 80");
}
#endif

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
                        startWebServer();
                        lastPacketTime = millis();
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
                startWebServer();
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
                // Drain everything queued this tick so a burst of frames can't
                // back up and stall the socket.
                int packetSize;
                while ((packetSize = udp.parsePacket()) > 0) {
                        lastPacketTime = millis();
                        if (packetSize == BUFFER_SIZE) {
                                if (udp.read(rxBuffer, BUFFER_SIZE) == BUFFER_SIZE &&
                                    !manualOverride) {
                                        processFramePacket(rxBuffer, BUFFER_SIZE);
                                        lastFrameTime = millis();
                                        currentState = STATE_ACTIVE;
                                }
                        } else if (packetSize >= 4 && packetSize < BUFFER_SIZE) {
                                uint8_t tempBuf[COMMAND_BUFFER_SIZE];
                                size_t bytesRead = udp.read(tempBuf, sizeof(tempBuf));
                                processCommandPacket(tempBuf, bytesRead);
                        } else {
                                udp.flush();
                        }
                }

                // Self-heal a wedged socket: WiFi can stay connected (and even
                // answer ping) while the UDP socket stops delivering. Rebind it
                // after a period of silence so the strip recovers without a
                // power cycle.
                if (lastPacketTime != 0 && millis() - lastPacketTime > RX_WATCHDOG_MS) {
                        Serial.println("UDP silent, rebinding socket");
                        udp.stop();
                        udp.begin(UDP_PORT);
                        lastPacketTime = millis();
                }

                // Serve the phone control page. handleClient() returns quickly
                // so it never disturbs the ~100Hz LED animation below.
                webServer.handleClient();
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
                uint8_t tempBuf[COMMAND_BUFFER_SIZE];
                size_t bytesRead = Serial.readBytes(tempBuf, sizeof(tempBuf));
                processCommandPacket(tempBuf, bytesRead);
                while (Serial.available())
                        Serial.read();
        }
#endif

        // Animate independently of packet arrival so jitter/bursts don't show
        // up as choppy motion. Frames only update the targets above.
        if (currentState == STATE_ACTIVE || currentState == STATE_STATIC) {
                unsigned long now = millis();
                unsigned long dt = now - lastAnimationTime;
                if (dt >= LED_ANIM_INTERVAL_MS) {
                        float alpha = 1.0f - expf(-(float)dt / LED_SMOOTHING_TAU_MS);
                        if (alpha > 1.0f)
                                alpha = 1.0f;
                        lastAnimationTime = now;
                        updateLEDs(alpha);
                }
        } else {
                lastAnimationTime = millis();
        }

        yield();
}
