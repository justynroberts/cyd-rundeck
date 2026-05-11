#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "storage.h"
#include "wifi_setup.h"
#include "rundeck.h"
#include "ui.h"

#include <WiFi.h>
#include <time.h>

// ===========================================================================
// Orchestration:
//   1. Display + LVGL boot, splash.
//   2. Boot-time factory-reset gesture (hold touch).
//   3. WiFi: try saved -> fall back to captive portal (Stage 1).
//   4. Rundeck creds: present? -> dashboard loop. Missing? -> Stage 2 setup.
//   5. Poll snapshot every RD_POLL_INTERVAL_MS on core 0 task; render on core 1.
// ===========================================================================

enum class App : uint8_t {
    Boot,
    Wifi,
    Setup,
    Running,
};

static volatile App s_app = App::Boot;
static volatile bool s_snapPending = false;
static volatile bool s_forcePoll  = false;
static rundeck::Snapshot s_lastSnap;
static SemaphoreHandle_t s_snapMutex;

// Pending messages from the WiFi task (core 0). The LVGL thread (core 1)
// drains these in the main loop — never touch LVGL from the WiFi task.
static SemaphoreHandle_t s_uiMutex;
static volatile bool s_pendingPortal = false;
static String s_pendingSsid, s_pendingPass;
static volatile bool s_pendingStatus = false;
static String s_pendingStatusMsg;

static void onPortalEnter(const String& ssid, const String& pass) {
    xSemaphoreTake(s_uiMutex, portMAX_DELAY);
    s_pendingSsid = ssid;
    s_pendingPass = pass;
    s_pendingPortal = true;
    xSemaphoreGive(s_uiMutex);
}

static void onStatus(const String& msg) {
    Serial.printf("[net] %s\n", msg.c_str());
    xSemaphoreTake(s_uiMutex, portMAX_DELAY);
    s_pendingStatusMsg = msg;
    s_pendingStatus = true;
    xSemaphoreGive(s_uiMutex);
}

// Polls Rundeck on core 0; LVGL on core 1 reads the shared snapshot.
static void rundeckTask(void*) {
    String pendingErr;
    for (;;) {
        if (storage::hasConfig() && netcfg::isConnected()) {
            rundeck::Snapshot s = rundeck::fetchSnapshot();
            if (xSemaphoreTake(s_snapMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                s_lastSnap = s;
                s_snapPending = true;
                xSemaphoreGive(s_snapMutex);
            }
        }
        // Honour a forced refresh from the UI thread; otherwise sleep in
        // short slices so we can wake up early.
        const uint32_t step = 200;
        uint32_t waited = 0;
        while (waited < RD_POLL_INTERVAL_MS) {
            if (s_forcePoll) { s_forcePoll = false; break; }
            vTaskDelay(pdMS_TO_TICKS(step));
            waited += step;
        }
    }
}

void requestImmediateRefresh() { s_forcePoll = true; }

static void startNtp() {
    // Try a few public NTP servers; non-blocking — first valid time wins.
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n[boot] cyd-rundeck");

    display::begin();

    // Factory reset gesture BEFORE LVGL takes over the screen redraw cycle.
    if (display::factoryResetPrompt(2000)) {
        storage::wipeAll();
        delay(500);
        ESP.restart();
    }

    storage::begin();
    ui::begin();
    ui::setRefreshCallback([]() { s_forcePoll = true; });
    ui::showSplash();
    ui::showStatus("Booting...");

    s_snapMutex = xSemaphoreCreateMutex();
    s_uiMutex   = xSemaphoreCreateMutex();

    // Kick WiFi (saved-creds attempt with retry, then portal if needed)
    netcfg::begin(onPortalEnter, onStatus);
}

static bool s_ntpStarted = false;
static bool s_setupStarted = false;
static uint32_t s_dashboardShownAt = 0;

void loop() {
    display::tick();
    ui::tick();
    netcfg::process();

    // Drain queued events from the WiFi task (core 0). Touching LVGL here
    // is safe because loop() runs on core 1.
    if (s_pendingPortal || s_pendingStatus) {
        String ssid, pass, status;
        bool portal = false, st = false;
        if (xSemaphoreTake(s_uiMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_pendingPortal) {
                ssid = s_pendingSsid; pass = s_pendingPass;
                s_pendingPortal = false; portal = true;
            }
            if (s_pendingStatus) {
                status = s_pendingStatusMsg;
                s_pendingStatus = false; st = true;
            }
            xSemaphoreGive(s_uiMutex);
        }
        if (portal) { s_app = App::Wifi; ui::showWifiPortal(ssid, pass); }
        if (st)     { ui::showStatus(status); }
    }

    if (!netcfg::isConnected()) {
        // Wifi/portal stage — UI already showing the right screen via callback.
        if (s_app != App::Wifi && netcfg::isPortalActive()) {
            // covered by onPortalEnter
        }
        delay(5);
        return;
    }

    // WiFi up.
    if (!s_ntpStarted) {
        startNtp();
        s_ntpStarted = true;
    }

    // Start the admin/setup portal once WiFi is up. If no creds yet, also
    // show the setup screen on the CYD; otherwise the portal sits quietly
    // in the background as an admin console (http://rundeck.local).
    if (!s_setupStarted) {
        netcfg::startSetupPortal(nullptr, onStatus);
        s_setupStarted = true;
    }
    if (!storage::hasConfig()) {
        if (s_app != App::Setup) {
            String url = String("http://rundeck.local  /  ") + WiFi.localIP().toString();
            ui::showSetupPortal(url);
            s_app = App::Setup;
        }
        delay(5);
        return;
    }

    // Keep the setup portal running as an admin console even after config
    // is saved — so the user can change the project filter / token without
    // a factory reset. http://rundeck.local stays reachable.
    (void)s_setupStarted;

    // First time we enter "Running" — show the dashboard, kick the poll task.
    if (s_app != App::Running) {
        ui::setServerInfo(storage::baseUrl(), "", storage::project());
        ui::showDashboard();
        ui::releaseTransientScreens();
        s_app = App::Running;
        s_dashboardShownAt = millis();

        // Kick the poll task once.
        static bool taskCreated = false;
        if (!taskCreated) {
            xTaskCreatePinnedToCore(rundeckTask, "rd", 8192, nullptr, 1, nullptr, 0);
            taskCreated = true;
        }
    }

    // Drain any pending snapshot from the poll task.
    if (s_snapPending) {
        if (xSemaphoreTake(s_snapMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            rundeck::Snapshot s = s_lastSnap;
            s_snapPending = false;
            xSemaphoreGive(s_snapMutex);
            ui::setSnapshot(s);
        }
    }

    delay(5);
}
