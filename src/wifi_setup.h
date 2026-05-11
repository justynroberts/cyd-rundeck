#pragma once
#include <Arduino.h>
#include <functional>

namespace netcfg {
    using PortalEnterCb = std::function<void(const String& ssid, const String& pass)>;
    using StatusCb      = std::function<void(const String&)>;

    void   begin(PortalEnterCb onPortal, StatusCb onStatus);
    void   process();
    String apSsid();
    bool   isConnected();
    bool   isPortalActive();

    // The portal also serves the Rundeck setup form once WiFi is up and creds
    // are missing. Started independently on demand.
    void   startSetupPortal(PortalEnterCb onPortal, StatusCb onStatus);
    void   stopSetupPortal();
    bool   isSetupPortalActive();
}
