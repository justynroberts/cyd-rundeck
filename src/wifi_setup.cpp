#include "wifi_setup.h"
#include "config.h"
#include "storage.h"
#include "rundeck.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ===========================================================================
// Two-stage self-contained portal:
//   Stage A — WiFi setup (in AP mode, captive)
//   Stage B — Rundeck base URL + token (in STA mode, served at rundeck.local)
// Reuses lessons from PD-CYD: own scan loop, manual SSID input, magic-URL
// catchers, retry on first-boot timeout. WiFiManager is intentionally avoided.
// ===========================================================================

static String g_apSsid;
static netcfg::PortalEnterCb s_onPortal;
static netcfg::StatusCb      s_onStatus;
static volatile bool g_apActive  = false;
static volatile bool g_connected = false;
static volatile bool g_setupActive = false;
static volatile bool g_setupDone   = false;

static const uint16_t DNS_PORT = 53;
static DNSServer  dns;
static WebServer  http(80);
static String     g_lastError;
static String     g_setupError;
static String     g_setupOk;

// ---- shared HTML chrome ---------------------------------------------------

// Rundeck-flavored CSS: dark navy bg, blue accent, IBM Plex Sans web font.
// The font loads from Google Fonts CDN; if offline (captive portal stage), we
// fall back to the platform sans stack and the page still looks good.
static const char CSS[] PROGMEM =
    "@import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600;700&family=IBM+Plex+Mono:wght@500&display=swap');"
    "*{box-sizing:border-box}"
    "body{font-family:'IBM Plex Sans',-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "background:radial-gradient(1200px 600px at 50% -200px,#1a2d4a 0%,#0f1a2b 60%) fixed;"
    "color:#e7eef7;margin:0;padding:32px 16px;min-height:100vh;font-weight:400;"
    "-webkit-font-smoothing:antialiased;letter-spacing:.005em}"
    ".w{max-width:560px;margin:0 auto}"
    ".brand{display:flex;align-items:center;gap:12px;margin-bottom:2px}"
    ".brand svg{width:32px;height:32px;flex:0 0 32px}"
    "h1{font-size:26px;margin:0;color:#fff;font-weight:600;letter-spacing:-0.01em;line-height:1.15}"
    ".sub{color:#7d93b2;font-size:13px;margin:8px 0 22px;font-weight:400}"
    ".card{background:linear-gradient(180deg,#1a2b44 0%,#15243a 100%);"
    "border:1px solid #25395b;border-radius:14px;padding:22px;margin-bottom:14px;"
    "box-shadow:0 8px 30px rgba(0,0,0,.35),0 1px 0 rgba(255,255,255,.04) inset}"
    "label{display:block;font-size:11px;letter-spacing:.14em;text-transform:uppercase;"
    "color:#7d93b2;margin:14px 0 6px;font-weight:600}"
    "label:first-child{margin-top:0}"
    "input,select{font-family:inherit;width:100%;padding:12px 14px;border-radius:9px;"
    "border:1px solid #25395b;background:#0c1626;color:#e7eef7;font-size:15px;"
    "outline:none;font-weight:500;transition:border-color .15s,box-shadow .15s}"
    "input:focus,select:focus{border-color:#4a90e2;box-shadow:0 0 0 3px rgba(74,144,226,.22)}"
    "input::placeholder{color:#506689;font-weight:400}"
    "button{font-family:inherit;appearance:none;border:0;"
    "background:linear-gradient(180deg,#4a90e2 0%,#2f7bd0 100%);"
    "color:#fff;font-weight:600;padding:13px;border-radius:9px;width:100%;font-size:14px;"
    "letter-spacing:.06em;cursor:pointer;margin-top:18px;text-transform:uppercase;"
    "box-shadow:0 4px 12px rgba(74,144,226,.32);transition:transform .08s,box-shadow .15s}"
    "button.secondary{background:#1f3554;color:#cfe0ff;font-weight:500;margin-top:10px;"
    "text-transform:none;letter-spacing:0;box-shadow:none}"
    "button:hover{filter:brightness(1.08)}button:active{transform:translateY(1px)}"
    ".banner{padding:11px 14px;border-radius:9px;margin-bottom:14px;font-size:13px;font-weight:500}"
    ".ok{background:#0f2b1a;color:#aef0c0;border:1px solid #1f5532}"
    ".err{background:#2a1011;color:#ffb6bb;border:1px solid #5a1d22}"
    ".small{color:#7d93b2;font-size:12px;margin-top:8px;line-height:1.5}"
    ".scan-state{color:#7d93b2;font-size:12px;margin:8px 0 0}"
    ".step{display:inline-block;padding:5px 10px;font-size:10px;letter-spacing:.18em;"
    "background:rgba(74,144,226,.14);color:#9bc4f1;border:1px solid rgba(74,144,226,.3);"
    "border-radius:99px;margin-bottom:14px;text-transform:uppercase;font-weight:600}"
    "code,kbd{font-family:'IBM Plex Mono',ui-monospace,SFMono-Regular,Menlo,monospace;"
    "font-size:.92em;background:#0c1626;border:1px solid #25395b;padding:2px 6px;border-radius:5px}";

static const char GEAR_SVG[] PROGMEM =
    "<svg viewBox='0 0 24 24' fill='none' stroke='#4a90e2' stroke-width='2' "
    "stroke-linecap='round' stroke-linejoin='round'>"
    "<circle cx='12' cy='12' r='3'/>"
    "<path d='M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.9 2.9l-.1-.1"
    "a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5"
    "1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.9-2.9l.1-.1a1.7 1.7 0 0 0 .3-1.8"
    "1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8"
    "l-.1-.1a2 2 0 1 1 2.9-2.9l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3"
    "a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.9 2.9"
    "l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1"
    "a1.7 1.7 0 0 0-1.5 1z'/></svg>";

static String htmlEscape(const String& s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i];
        switch(c){case '<':o+="&lt;";break;case '>':o+="&gt;";break;
        case '&':o+="&amp;";break;case '"':o+="&quot;";break;default:o+=c;}}
    return o;
}

static String makeApSsid() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", (uint16_t)(mac & 0xFFFF));
    return String(AP_SSID_PREFIX) + buf;
}

// ---- WiFi portal pages ----------------------------------------------------

static String wifiFormPage(const String& msg, bool err) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED || n == -2) {
        WiFi.scanNetworks(true, false);
        n = WIFI_SCAN_RUNNING;
    }

    String h;
    h.reserve(4096);
    h += F("<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Rundeck CYD - WiFi</title><style>");
    h += FPSTR(CSS);
    h += F("</style></head><body><div class=\"w\">"
           "<div class=\"brand\">");
    h += FPSTR(GEAR_SVG);
    h += F("<h1>Rundeck CYD</h1></div>"
           "<div class=\"sub\">Step 1 of 2 &middot; Connect to WiFi</div>"
           "<div class=\"step\">STEP 1 / 2 &middot; WIFI</div>");

    if (msg.length()) {
        h += "<div class=\"banner ";
        h += err ? "err" : "ok";
        h += "\">";
        h += htmlEscape(msg);
        h += "</div>";
    }

    h += F("<form method=\"POST\" action=\"/wifi\" class=\"card\">"
           "<label>Network</label><select name=\"ssid\">"
           "<option value=\"\">-- pick a network or type manually below --</option>");
    if (n > 0) {
        for (int i = 0; i < n && i < 25; ++i) {
            String s = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            h += "<option value=\"" + htmlEscape(s) + "\">"
                 + htmlEscape(s) + " (" + String(rssi) + " dBm)</option>";
        }
    }
    h += F("</select>");
    if (n == WIFI_SCAN_RUNNING) {
        h += "<p class=\"scan-state\">Scanning networks&hellip; refresh in a few seconds.</p>";
    } else if (n == 0) {
        h += "<p class=\"scan-state\">No networks found. Try rescanning, or type your SSID manually.</p>";
    } else if (n > 0) {
        h += "<p class=\"scan-state\">" + String(n) + " network" + String(n==1?"":"s") + " found.</p>";
    }
    h += F("<label>Or type SSID manually <small>(hidden / not listed)</small></label>"
           "<input type=\"text\" name=\"ssid_manual\" placeholder=\"My WiFi\" autocomplete=\"off\">"
           "<label>Password</label>"
           "<input type=\"password\" name=\"pass\" autocomplete=\"new-password\">"
           "<button type=\"submit\">Save &amp; Connect</button>"
           "<div class=\"small\">2.4 GHz only. ESP32 can't do 5 GHz or WPA3-only networks.</div>"
           "</form>"
           "<form method=\"POST\" action=\"/rescan\" class=\"card\" style=\"padding:14px 18px\">"
           "<button type=\"submit\" class=\"secondary\">Rescan networks</button>"
           "</form>"
           "</div></body></html>");
    return h;
}

// ---- Rundeck setup page ---------------------------------------------------

static String rundeckSetupPage() {
    String base = storage::baseUrl();
    String proj = storage::project();

    String h;
    h.reserve(4096);
    h += F("<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Rundeck CYD - Setup</title><style>");
    h += FPSTR(CSS);
    h += F("</style></head><body><div class=\"w\">"
           "<div class=\"brand\">");
    h += FPSTR(GEAR_SVG);
    h += F("<h1>Rundeck CYD</h1></div>"
           "<div class=\"sub\">Step 2 of 2 &middot; Rundeck server</div>"
           "<div class=\"step\">STEP 2 / 2 &middot; RUNDECK</div>");

    if (g_setupError.length()) {
        h += "<div class=\"banner err\">" + htmlEscape(g_setupError) + "</div>";
        g_setupError = "";
    }
    if (g_setupOk.length()) {
        h += "<div class=\"banner ok\">" + htmlEscape(g_setupOk) + "</div>";
        g_setupOk = "";
    }

    bool haveToken = storage::token().length() > 0;
    h += F("<form method=\"POST\" action=\"/rundeck\" class=\"card\">"
           "<label>Rundeck base URL</label>"
           "<input type=\"url\" name=\"baseurl\" required placeholder=\"https://rundeck.example.com\" value=\"");
    h += htmlEscape(base);
    h += F("\">"
           "<label>API token</label>"
           "<input type=\"password\" name=\"token\" placeholder=\"");
    h += haveToken ? F("(leave blank to keep current)") : F("X-Rundeck-Auth-Token value");
    h += F("\">"
           "<div class=\"small\">User profile &rarr; User API Tokens (Rundeck UI)</div>"
           "<label>Project filter <small>(optional)</small></label>"
           "<input type=\"text\" name=\"project\" placeholder=\"leave blank for all projects\" value=\"");
    h += htmlEscape(proj);
    h += F("\">"
           "<button type=\"submit\">Save</button>"
           "<div class=\"small\">Settings are applied on the next poll (15s).</div>"
           "</form>"
           "</div></body></html>");
    return h;
}

// ---- handlers (wifi portal) -----------------------------------------------

static void handleWifiRoot()  {
    http.send(200, "text/html", wifiFormPage(g_lastError, g_lastError.length() > 0));
    g_lastError = "";
}
static void redirectRoot()    { http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); }
static void handleRescan()    {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
    http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
}

static void saveWifiCreds(const String& ssid, const String& pass) {
    Preferences p; p.begin("wifi_cfg", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}
static bool loadWifiCreds(String& ssid, String& pass) {
    Preferences p; p.begin("wifi_cfg", true);
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

static bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
    Serial.printf("[wifi] connecting to '%s' ...\n", ssid.c_str());
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            Serial.printf("[wifi] connected ip=%s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            Serial.printf("[wifi] connect failed status=%d\n", (int)st);
            break;
        }
        delay(150);
    }
    Serial.printf("[wifi] connect timed out, last status=%d\n", (int)WiFi.status());
    return false;
}

static void handleWifiSave() {
    String ssid = http.arg("ssid_manual");
    if (ssid.length() == 0) ssid = http.arg("ssid");
    String pass = http.arg("pass");
    ssid.trim();

    if (ssid.length() == 0) {
        g_lastError = "SSID is empty.";
        http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
        return;
    }

    String pre  = String("Connecting to ") + ssid + "...";
    String body =
        String("<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"6;url=/\">"
               "<style>") + FPSTR(CSS) + String("</style></head>"
               "<body><div class=\"w\"><div class=\"brand\">") + FPSTR(GEAR_SVG) +
        String("<h1>Rundeck CYD</h1></div>"
               "<div class=\"card\"><h1 style=\"color:#4a90e2;font-size:18px\">") + htmlEscape(pre) +
        String("</h1><p>Saved. The device will switch off this WiFi to join the target network.</p></div></div></body></html>");
    http.send(200, "text/html", body);
    delay(200);

    saveWifiCreds(ssid, pass);
    dns.stop();
    http.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_apActive = false;

    if (tryConnect(ssid, pass, 25000)) {
        g_connected = true;
        if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
        return;
    }
    Serial.println("[wifi] connect failed, rebooting to retry portal");
    delay(800);
    ESP.restart();
}

// ---- handlers (rundeck setup) ---------------------------------------------

static void handleSetupRoot() {
    http.send(200, "text/html", rundeckSetupPage());
}

static void handleSetupSave() {
    String base = http.arg("baseurl");
    String tok  = http.arg("token");
    String proj = http.arg("project");
    base.trim(); tok.trim(); proj.trim();

    // Allow leaving the token blank to keep the existing one — useful when
    // the user just wants to change the project filter or base URL.
    String existingTok = storage::token();
    if (tok.length() == 0) tok = existingTok;

    if (base.length() == 0 || tok.length() == 0) {
        g_setupError = "Base URL and token are required (token can be blank only if one is already saved).";
        http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
        return;
    }

    // Save first. We deliberately do NOT TLS-validate here — the handshake
    // runs on the main loop task and is too heavy to do inline (heap +
    // stack pressure caused crashes). The dashboard polls right after and
    // any auth error surfaces in the footer.
    storage::setBaseUrl(base);
    storage::setToken(tok);
    storage::setProject(proj);
    g_setupDone = true;

    String body =
        String("<html><head><meta charset=\"utf-8\"><style>") + FPSTR(CSS) +
        String("</style></head><body><div class=\"w\"><div class=\"brand\">") + FPSTR(GEAR_SVG) +
        String("<h1>Rundeck CYD</h1></div><div class=\"card\"><div class=\"banner ok\">"
               "Saved. The device is connecting to Rundeck now — watch the "
               "screen footer for any errors. You can close this tab."
               "</div></div></div></body></html>");
    http.send(200, "text/html", body);
}

// ---- portal loops ---------------------------------------------------------

static void portalLoop() {
    g_apActive = true;
    Serial.printf("[wifi] AP up: %s ip=%s\n",
                  g_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    if (s_onPortal) s_onPortal(g_apSsid, AP_PASSWORD);

    WiFi.scanNetworks(true, false, false, 300);

    dns.start(DNS_PORT, "*", WiFi.softAPIP());

    http.on("/",                HTTP_GET,  handleWifiRoot);
    http.on("/wifi",            HTTP_POST, handleWifiSave);
    http.on("/rescan",          HTTP_POST, handleRescan);
    http.on("/generate_204",    HTTP_GET,  redirectRoot);
    http.on("/gen_204",         HTTP_GET,  redirectRoot);
    http.on("/hotspot-detect.html", HTTP_GET, handleWifiRoot);
    http.on("/library/test/success.html", HTTP_GET, handleWifiRoot);
    http.on("/connecttest.txt", HTTP_GET, redirectRoot);
    http.on("/ncsi.txt",        HTTP_GET, redirectRoot);
    http.on("/redirect",        HTTP_GET, redirectRoot);
    http.onNotFound([](){ http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); });
    http.begin();

    uint32_t lastScanKick = millis();
    while (!g_connected) {
        dns.processNextRequest();
        http.handleClient();
        if (millis() - lastScanKick > 8000) {
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_FAILED || n == -2 || n == 0) {
                WiFi.scanDelete();
                WiFi.scanNetworks(true, false, false, 300);
            }
            lastScanKick = millis();
        }
        delay(2);
    }
}

static void wifiTask(void*) {
    if (s_onStatus) s_onStatus("Connecting WiFi...");

    String ssid, pass;
    if (loadWifiCreds(ssid, pass)) {
        if (s_onStatus) s_onStatus(String("Connecting to ") + ssid);
        WiFi.mode(WIFI_STA);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt > 0) { Serial.printf("[wifi] retry %d\n", attempt); delay(1500); }
            if (tryConnect(ssid, pass, 25000)) {
                g_connected = true;
                if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
                vTaskDelete(NULL);
                return;
            }
        }
        Serial.println("[wifi] saved creds failed after retries, opening portal");
    } else {
        Serial.println("[wifi] no saved creds, opening portal");
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD);
    delay(200);
    portalLoop();

    if (s_onStatus) s_onStatus("Connected");
    vTaskDelete(NULL);
}

// ---- public API -----------------------------------------------------------

void netcfg::begin(PortalEnterCb onPortal, StatusCb onStatus) {
    g_apSsid   = makeApSsid();
    s_onPortal = onPortal;
    s_onStatus = onStatus;
    g_connected = false;
    g_apActive  = false;
    xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, nullptr, 1, nullptr, 0);
}

void netcfg::process() {
    if (!g_connected && WiFi.status() == WL_CONNECTED) g_connected = true;
    if (g_setupActive) http.handleClient();
}

String netcfg::apSsid()         { return g_apSsid.length() ? g_apSsid : makeApSsid(); }
bool   netcfg::isConnected()    { return g_connected || WiFi.status() == WL_CONNECTED; }
bool   netcfg::isPortalActive() { return g_apActive; }
bool   netcfg::isSetupPortalActive() { return g_setupActive; }

void netcfg::startSetupPortal(PortalEnterCb /*onPortal*/, StatusCb onStatus) {
    if (g_setupActive) return;
    g_setupDone = false;

    // mDNS so the user can hit http://rundeck.local/
    if (MDNS.begin("rundeck")) {
        MDNS.addService("http", "tcp", 80);
    }

    http.stop();
    http.on("/",         HTTP_GET,  handleSetupRoot);
    http.on("/rundeck",  HTTP_POST, handleSetupSave);
    http.onNotFound([](){ http.sendHeader("Location", "/"); http.send(302, "text/plain", ""); });
    http.begin();

    g_setupActive = true;
    if (onStatus) onStatus(String("http://rundeck.local  or  ") + WiFi.localIP().toString());
}

void netcfg::stopSetupPortal() {
    if (!g_setupActive) return;
    http.stop();
    MDNS.end();
    g_setupActive = false;
}
