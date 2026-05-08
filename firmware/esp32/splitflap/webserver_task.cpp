/*
   SplitFlap Shop Controller - Web Server Task

   See webserver_task.h for an overview.

   Based on the splitflap project by Scott Bezek (Apache 2.0 License)
*/
#if HTTP
#include "webserver_task.h"

#include <lwip/apps/sntp.h>

#include "secrets.h"

// ─── Constructor ─────────────────────────────────────────────
WebServerTask::WebServerTask(
    SplitflapTask& splitflap_task,
    DisplayTask& display_task,
    Logger& logger,
    const uint8_t task_core
) :
    Task("WebServer", 8192, 1, task_core),
    splitflap_task_(splitflap_task),
    display_task_(display_task),
    logger_(logger),
    server_(80)
{
}

// ─── WiFi Connection ─────────────────────────────────────────
void WebServerTask::connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Disable WiFi sleep — causes glitches on pin 39 used by Chainlink sensors.
    // See: https://github.com/espressif/arduino-esp32/issues/4903
    WiFi.setSleep(WIFI_PS_NONE);

    char buf[256];
    logger_.log("Connecting to WiFi...");
    snprintf(buf, sizeof(buf), "WiFi: %s", WIFI_SSID);
    display_task_.setMessage(1, String(buf));

    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
    }

    snprintf(buf, sizeof(buf), "Connected! IP: %s", WiFi.localIP().toString().c_str());
    logger_.log(buf);

    snprintf(buf, sizeof(buf), "http://%s", WiFi.localIP().toString().c_str());
    display_task_.setMessage(1, String(buf));
}

// ─── SPIFFS (UI filesystem) ────────────────────────────────────
void WebServerTask::setupFS() {
    // begin(formatOnFail=true) — first boot will format if SPIFFS is empty.
    if (!SPIFFS.begin(true)) {
        logger_.log("SPIFFS mount failed even after format — UI hosting disabled");
        fs_mounted_ = false;
        return;
    }
    fs_mounted_ = true;

    char buf[128];
    snprintf(buf, sizeof(buf), "SPIFFS mounted: %u / %u bytes used",
             (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    logger_.log(buf);

    if (!SPIFFS.exists("/index.html")) {
        logger_.log("WARN: /index.html not present on SPIFFS — run PlatformIO 'Upload Filesystem Image'");
    }
}

// ─── mDNS ────────────────────────────────────────────────────
void WebServerTask::setupMDNS() {
    // DEVICE_INSTANCE_NAME comes from secrets.h (e.g. "reedboard").
    // After this, http://<name>.local resolves on the LAN.
    if (MDNS.begin(DEVICE_INSTANCE_NAME)) {
        MDNS.addService("http", "tcp", 80);
        char buf[128];
        snprintf(buf, sizeof(buf), "mDNS: http://%s.local", DEVICE_INSTANCE_NAME);
        logger_.log(buf);
    } else {
        logger_.log("mDNS start failed (non-fatal)");
    }
}

// ─── ArduinoOTA ──────────────────────────────────────────────
void WebServerTask::setupOTA() {
    ArduinoOTA.setHostname(DEVICE_INSTANCE_NAME);
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

    ArduinoOTA
        .onStart([this]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
            char buf[64];
            snprintf(buf, sizeof(buf), "OTA start: %s", type.c_str());
            logger_.log(buf);
            // If updating filesystem, unmount first so writes succeed.
            if (ArduinoOTA.getCommand() == U_SPIFFS) {
                SPIFFS.end();
            }
            display_task_.setMessage(0, "OTA: " + type);
        })
        .onEnd([this]() {
            logger_.log("OTA complete — rebooting");
            display_task_.setMessage(0, "OTA done");
        })
        .onProgress([this](unsigned int progress, unsigned int total) {
            // Don't spam the log; just update the LCD occasionally.
            static unsigned last_pct = 255;
            unsigned pct = (progress * 100) / total;
            if (pct != last_pct && pct % 10 == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "OTA %u%%", pct);
                display_task_.setMessage(0, String(buf));
                last_pct = pct;
            }
        })
        .onError([this](ota_error_t error) {
            char buf[64];
            snprintf(buf, sizeof(buf), "OTA error: %u", (unsigned)error);
            logger_.log(buf);
            display_task_.setMessage(0, String(buf));
        });

    ArduinoOTA.begin();
    logger_.log("OTA ready");
}

// ─── Route Handlers ──────────────────────────────────────────

void WebServerTask::handleIndex() {
    server_.sendHeader("Access-Control-Allow-Origin", "*");

    if (!fs_mounted_ || !SPIFFS.exists("/index.html")) {
        // Friendly fallback so a fresh device isn't a black hole.
        String body =
            "<!doctype html><html><body style='font-family:sans-serif;padding:24px'>"
            "<h2>ReedBoard firmware running, UI not yet uploaded.</h2>"
            "<p>Run <code>pio run -t uploadfs</code> (USB) or "
            "<code>pio run -e chainlink_ota -t uploadfs</code> (WiFi) "
            "to push <code>index.html</code> to SPIFFS.</p>"
            "<p>API endpoints are live: "
            "<code>GET /status</code>, <code>POST /message</code>.</p>"
            "</body></html>";
        server_.send(200, "text/html", body);
        return;
    }

    File f = SPIFFS.open("/index.html", "r");
    if (!f) {
        server_.send(500, "text/plain", "Failed to open /index.html");
        return;
    }
    // Light caching so phones don't re-fetch the 130KB blob on every nav.
    server_.sendHeader("Cache-Control", "public, max-age=300");
    server_.streamFile(f, "text/html");
    f.close();
}

void WebServerTask::handleStatus() {
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server_.send(200, "application/json",
        "{\"status\":\"ok\",\"modules\":" + String(NUM_MODULES) +
        ",\"hostname\":\"" + String(DEVICE_INSTANCE_NAME) + "\"}");
}

void WebServerTask::handleMessage() {
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");

    if (server_.method() == HTTP_OPTIONS) {
        server_.send(204);
        return;
    }

    String text = "";
    if (server_.hasArg("text")) {
        text = server_.arg("text");
    } else if (server_.hasArg("plain")) {
        text = server_.arg("plain");
    }

    if (text.length() == 0) {
        server_.send(400, "application/json", "{\"error\":\"No text provided\"}");
        return;
    }

    text.toUpperCase();

    char buf[200];
    snprintf(buf, sizeof(buf), "Received message: %s", text.c_str());
    logger_.log(buf);

    showOnDisplay(text);
    display_task_.setMessage(0, "Msg: " + text);

    server_.send(200, "application/json",
        "{\"status\":\"ok\",\"displayed\":\"" + text + "\"}");
}

void WebServerTask::handleNotFound() {
    server_.sendHeader("Access-Control-Allow-Origin", "*");

    if (server_.method() == HTTP_OPTIONS) {
        server_.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
        server_.send(204);
        return;
    }

    server_.send(404, "text/plain", "Not Found");
}

void WebServerTask::showOnDisplay(const String& text) {
    char display_buf[NUM_MODULES + 1];
    size_t len = strlcpy(display_buf, text.c_str(), sizeof(display_buf));
    if (len < NUM_MODULES) {
        memset(display_buf + len, ' ', NUM_MODULES - len);
    }
    display_buf[NUM_MODULES] = '\0';
    splitflap_task_.showString(display_buf, NUM_MODULES, false);
}

// ─── Route Setup ─────────────────────────────────────────────
void WebServerTask::setupRoutes() {
    // UI
    server_.on("/", HTTP_GET, [this]() { handleIndex(); });

    // API
    server_.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server_.on("/status", HTTP_OPTIONS, [this]() { handleStatus(); });

    server_.on("/message", HTTP_POST, [this]() { handleMessage(); });
    server_.on("/message", HTTP_OPTIONS, [this]() { handleMessage(); });
    server_.on("/message", HTTP_GET, [this]() {
        server_.sendHeader("Access-Control-Allow-Origin", "*");
        server_.send(200, "text/plain",
            "POST /message with 'text' parameter to display a message.\n"
            "Example: curl -X POST http://" + WiFi.localIP().toString() +
            "/message -d 'text=HELLO WORLD'");
    });

    server_.onNotFound([this]() { handleNotFound(); });
}

// ─── Main Run Loop ───────────────────────────────────────────
void WebServerTask::run() {
    connectWifi();
    setupFS();
    setupMDNS();
    setupOTA();
    setupRoutes();
    server_.begin();

    char buf[200];
    snprintf(buf, sizeof(buf), "Web server up at http://%s  (also http://%s.local)",
             WiFi.localIP().toString().c_str(), DEVICE_INSTANCE_NAME);
    logger_.log(buf);

    showOnDisplay("WE SHIP IT.");

    while (1) {
        ArduinoOTA.handle();
        server_.handleClient();

        String wifi_status;
        if (WiFi.status() == WL_CONNECTED) {
            wifi_status = "http://" + WiFi.localIP().toString();
        } else {
            wifi_status = "WiFi disconnected!";
            WiFi.reconnect();
        }
        display_task_.setMessage(1, wifi_status);

        delay(10);
    }
}
#endif
