#include "storage.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

// Ensure the namespace exists, so subsequent read-only opens don't log
// nvs_open NOT_FOUND on every call.
void storage::begin() {
    Preferences p;
    if (p.begin(NVS_NS, false)) p.end();
}

bool storage::hasConfig() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return false;
    bool ok = p.isKey(NVS_KEY_BASEURL) && p.isKey(NVS_KEY_TOKEN)
              && p.getString(NVS_KEY_BASEURL, "").length() > 0
              && p.getString(NVS_KEY_TOKEN, "").length() > 0;
    p.end();
    return ok;
}

String storage::baseUrl() {
    Preferences p; p.begin(NVS_NS, true);
    String s = p.getString(NVS_KEY_BASEURL, "");
    p.end();
    return s;
}
String storage::token() {
    Preferences p; p.begin(NVS_NS, true);
    String s = p.getString(NVS_KEY_TOKEN, "");
    p.end();
    return s;
}
String storage::project() {
    Preferences p; p.begin(NVS_NS, true);
    String s = p.getString(NVS_KEY_PROJECT, "");
    p.end();
    return s;
}

void storage::setBaseUrl(const String& u) {
    Preferences p; p.begin(NVS_NS, false);
    p.putString(NVS_KEY_BASEURL, u);
    p.end();
}
void storage::setToken(const String& t) {
    Preferences p; p.begin(NVS_NS, false);
    p.putString(NVS_KEY_TOKEN, t);
    p.end();
}
void storage::setProject(const String& proj) {
    Preferences p; p.begin(NVS_NS, false);
    p.putString(NVS_KEY_PROJECT, proj);
    p.end();
}

void storage::wipe() {
    Preferences p; p.begin(NVS_NS, false);
    p.clear();
    p.end();
}

void storage::wipeAll() {
    wipe();
    Preferences p; p.begin("wifi_cfg", false);
    p.clear();
    p.end();
    WiFi.disconnect(true, true);
}
