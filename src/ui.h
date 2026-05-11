#pragma once
#include "rundeck.h"

namespace ui {
    void begin();

    // Screen routing
    void showSplash();
    void showWifiPortal(const String& ssid, const String& pass);
    void showSetupPortal(const String& url);
    void showStatus(const String& msg);
    void showDashboard();           // first time: builds; later: noop
    void showExecutions();          // execution list screen
    void showDetail(long execId);   // detail for an execution

    // Data feed
    void setSnapshot(const rundeck::Snapshot& s);
    void setError(const String& msg);
    void setServerInfo(const String& baseUrl, const String& version, const String& project);

    // Hard-redraw safety
    void invalidate();

    // Called by the dashboard when the user requests an immediate refresh.
    using RefreshCb = void(*)();
    void setRefreshCallback(RefreshCb cb);

    // Free transient screens (splash, wifi-portal, setup-portal) once the
    // dashboard is up. Reclaims a few KB of LVGL pool.
    void releaseTransientScreens();

    // Per-loop tick: handles the EXEC subview auto-rotation.
    void tick();
}
