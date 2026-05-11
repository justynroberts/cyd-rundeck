#pragma once
#include <Arduino.h>

namespace storage {
    void begin();

    // Rundeck creds / settings.
    bool  hasConfig();          // true if base URL + token both set
    String baseUrl();           // e.g. "https://rundeck.example.com"
    String token();             // X-Rundeck-Auth-Token value
    String project();           // optional project filter, "" = all

    void  setBaseUrl(const String& u);
    void  setToken(const String& t);
    void  setProject(const String& p);

    void  wipe();               // clears Rundeck creds (not WiFi)
    void  wipeAll();            // clears Rundeck + WiFi creds
}
