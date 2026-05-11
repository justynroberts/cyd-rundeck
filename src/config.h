#pragma once

// Display: 320x240 landscape (rotation 1)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Touch (XPT2046) — separate SPI bus (VSPI)
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_SCLK 25
#define TOUCH_CS   33
#define TOUCH_IRQ  36

#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

// Onboard RGB LED (active-low)
#define LED_R 4
#define LED_G 16
#define LED_B 17

// Captive portal
#define AP_SSID_PREFIX "Rundeck-CYD-"
#define AP_PASSWORD    "rundeck1"

// Rundeck API
#define RD_POLL_INTERVAL_MS    15000   // dashboard refresh cadence
#define RD_API_VERSION         41      // minimum Rundeck API version we target
#define RD_HTTP_TIMEOUT_MS     8000
#define RD_MAX_EXECUTIONS      12      // bounded list to keep heap small
#define RD_METRICS_WINDOW      "24h"   // recentFilter window for /executions/metrics

// HTTP web portal
#define PORTAL_HTTP_PORT 80

// NVS
#define NVS_NS              "rdcyd"
#define NVS_KEY_BASEURL     "rd_url"      // e.g. https://rundeck.example.com
#define NVS_KEY_TOKEN       "rd_token"    // X-Rundeck-Auth-Token
#define NVS_KEY_PROJECT     "rd_project"  // optional project filter
