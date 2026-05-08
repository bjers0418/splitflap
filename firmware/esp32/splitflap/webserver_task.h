/*
   SplitFlap Shop Controller - Web Server Task

   Replaces the stock HTTP task with a web server that:
   - Connects to WiFi (STA mode) using credentials in secrets.h
   - Serves the ReedBoard app HTML at GET /  (from FFat filesystem)
   - Accepts POST /message  with text=... to drive the display
   - Exposes  GET /status   for heartbeat + module count
   - Supports ArduinoOTA so all future firmware/UI updates can be wireless
   - Advertises itself via mDNS as <DEVICE_INSTANCE_NAME>.local

   Based on the splitflap project by Scott Bezek (Apache 2.0 License)
*/
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "../core/logger.h"
#include "../core/splitflap_task.h"
#include "../core/task.h"

#include "display_task.h"

class WebServerTask : public Task<WebServerTask> {
    friend class Task<WebServerTask>;

    public:
        WebServerTask(SplitflapTask& splitflap_task, DisplayTask& display_task, Logger& logger, const uint8_t task_core);

    protected:
        void run();

    private:
        void connectWifi();
        void setupFS();
        void setupMDNS();
        void setupOTA();
        void setupRoutes();
        void handleIndex();
        void handleMessage();
        void handleStatus();
        void handleNotFound();
        void showOnDisplay(const String& text);

        SplitflapTask& splitflap_task_;
        DisplayTask& display_task_;
        Logger& logger_;
        WebServer server_;
        bool fs_mounted_ = false;
};
