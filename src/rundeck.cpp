#include "rundeck.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

using namespace rundeck;

// ----- helpers -----------------------------------------------------------

const char* rundeck::statusText(Status s) {
    switch (s) {
        case Status::Running:         return "RUNNING";
        case Status::Succeeded:       return "SUCCEEDED";
        case Status::Failed:          return "FAILED";
        case Status::Aborted:         return "ABORTED";
        case Status::TimedOut:        return "TIMEOUT";
        case Status::FailedWithRetry: return "RETRYING";
        case Status::Scheduled:       return "SCHEDULED";
        default:                      return "UNKNOWN";
    }
}

uint32_t rundeck::statusColor(Status s) {
    // Rundeck-flavoured palette (RGB888):
    switch (s) {
        case Status::Running:         return 0x4A90E2;  // Rundeck blue
        case Status::Succeeded:       return 0x4CAF50;  // green
        case Status::Failed:          return 0xE53935;  // red
        case Status::Aborted:         return 0x9E9E9E;  // grey
        case Status::TimedOut:        return 0xFF9800;  // orange
        case Status::FailedWithRetry: return 0xFFC107;  // amber
        case Status::Scheduled:       return 0x7E57C2;  // purple
        default:                      return 0x607D8B;  // blue-grey
    }
}

static Status parseStatus(const String& s) {
    if (s == "running")            return Status::Running;
    if (s == "succeeded")          return Status::Succeeded;
    if (s == "failed")             return Status::Failed;
    if (s == "aborted")            return Status::Aborted;
    if (s == "timedout")           return Status::TimedOut;
    if (s == "failed-with-retry")  return Status::FailedWithRetry;
    if (s == "scheduled")          return Status::Scheduled;
    return Status::Unknown;
}

// Convert Rundeck ISO8601 (with optional millis) -> unix seconds.
// Best-effort; returns 0 if parse fails. Stays UTC.
static uint32_t parseIso(const String& iso) {
    if (iso.length() < 19) return 0;
    int year, mon, day, hh, mm, ss;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
               &year, &mon, &day, &hh, &mm, &ss) != 6) return 0;
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    time_t epoch = mktime(&t);
    return epoch > 0 ? (uint32_t)epoch : 0;
}

// Build the Rundeck v41 endpoint URL: <base>/api/41/<path>
static String apiUrl(const String& base, const String& path) {
    String b = base;
    while (b.endsWith("/")) b.remove(b.length() - 1);
    String u = b + "/api/" + String(RD_API_VERSION) + path;
    return u;
}

// HTTPS GET with retry, returns body. Sets outStatus to HTTP code (or negative
// ESP error on transport failure). Caller frees nothing — String owns memory.
static bool httpGet(const String& url, const String& token,
                    int& outStatus, String& outBody, String& outErr) {
    WiFiClientSecure tls;
    tls.setInsecure();             // self-signed Rundeck is common in the wild
    HTTPClient http;
    http.setTimeout(RD_HTTP_TIMEOUT_MS);
    http.setReuse(false);

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) delay(600);
        if (!http.begin(tls, url)) {
            outErr = "begin() failed";
            continue;
        }
        http.addHeader("X-Rundeck-Auth-Token", token);
        http.addHeader("Accept", "application/json");
        int code = http.GET();
        outStatus = code;
        if (code > 0) {
            outBody = http.getString();
            http.end();
            if (code >= 200 && code < 300) return true;
            outErr = String("HTTP ") + code;
            // 401/403/404 are permanent — don't retry
            if (code == 401 || code == 403 || code == 404) return false;
        } else {
            outErr = String("transport ") + code;
            http.end();
        }
    }
    return false;
}

static bool httpPost(const String& url, const String& token,
                     const String& body, const String& contentType,
                     int& outStatus, String& outBody, String& outErr) {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setTimeout(RD_HTTP_TIMEOUT_MS);
    http.setReuse(false);

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) delay(600);
        if (!http.begin(tls, url)) { outErr = "begin() failed"; continue; }
        http.addHeader("X-Rundeck-Auth-Token", token);
        http.addHeader("Accept", "application/json");
        http.addHeader("Content-Type", contentType);
        int code = http.POST((uint8_t*)body.c_str(), body.length());
        outStatus = code;
        if (code > 0) {
            outBody = http.getString();
            http.end();
            if (code >= 200 && code < 300) return true;
            outErr = String("HTTP ") + code;
            if (code == 401 || code == 403 || code == 404) return false;
        } else {
            outErr = String("transport ") + code;
            http.end();
        }
    }
    return false;
}

// ----- public API ---------------------------------------------------------

bool rundeck::validate(const String& baseUrl, const String& token,
                       String& outErr, String& outVersion) {
    int code = 0;
    String body, err;
    String url = apiUrl(baseUrl, "/system/info");
    if (!httpGet(url, token, code, body, err)) {
        outErr = err.length() ? err : "request failed";
        return false;
    }
    JsonDocument filter;
    filter["system"]["rundeck"]["version"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filter));
    if (e) { outErr = String("json: ") + e.c_str(); return false; }
    const char* v = doc["system"]["rundeck"]["version"].as<const char*>();
    outVersion = v ? String(v) : String("unknown");
    return true;
}

// Pick the first project the token can see. Used when no explicit project
// filter is configured — Rundeck v41 doesn't expose a cross-project /executions.
static String firstAvailableProject(const String& base, const String& tok) {
    int code = 0;
    String body, err;
    if (!httpGet(apiUrl(base, "/projects"), tok, code, body, err)) return String();
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return String();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject p : arr) {
        const char* n = p["name"].as<const char*>();
        if (n && *n) return String(n);
    }
    return String();
}

Snapshot rundeck::fetchSnapshot() {
    Snapshot snap;
    snap.ok = false;
    snap.running = 0;
    snap.succeededRecent = 0;
    snap.failedRecent = 0;

    String base = storage::baseUrl();
    String tok  = storage::token();
    String proj = storage::project();
    if (base.length() == 0 || tok.length() == 0) {
        snap.error = "no config";
        return snap;
    }

    // Resolve effective project (auto-pick first if no filter set).
    String effProj = proj;
    if (effProj.length() == 0) {
        effProj = firstAvailableProject(base, tok);
        if (effProj.length() == 0) {
            snap.error = "no project visible to this token";
            return snap;
        }
    }

    // 1) running executions for the project
    String path = "/project/" + effProj + "/executions/running?max=" + String(RD_MAX_EXECUTIONS);
    int code = 0;
    String body, err;
    if (!httpGet(apiUrl(base, path), tok, code, body, err)) {
        snap.error = err;
        return snap;
    }

    auto pushExec = [&](JsonObject ex, Status forcedStatus, bool useForced) {
        Execution e2;
        e2.id      = ex["id"].as<long>();
        // Dedup
        for (auto& already : snap.recent) if (already.id == e2.id) return;
        Status st = useForced ? forcedStatus : parseStatus(
            ex["status"].as<const char*>() ? String(ex["status"].as<const char*>()) : String(""));
        e2.status  = st;
        e2.project = ex["project"].as<const char*>() ? ex["project"].as<const char*>() : "";
        const char* jn = ex["job"]["name"];
        e2.jobName = jn ? String(jn) : String("adhoc");
        e2.user    = ex["user"].as<const char*>() ? ex["user"].as<const char*>() : "";
        e2.startedEpoch = parseIso(ex["date-started"]["date"].as<const char*>()
                                   ? String(ex["date-started"]["date"].as<const char*>()) : String(""));
        e2.endedEpoch   = parseIso(ex["date-ended"]["date"].as<const char*>()
                                   ? String(ex["date-ended"]["date"].as<const char*>()) : String(""));
        e2.durationSec  = (e2.startedEpoch && e2.endedEpoch)
                          ? (long)(e2.endedEpoch - e2.startedEpoch) : -1;
        if (st == Status::Succeeded) snap.succeededRecent++;
        if (st == Status::Failed || st == Status::TimedOut || st == Status::FailedWithRetry)
            snap.failedRecent++;
        snap.recent.push_back(e2);
    };

    {
        JsonDocument doc;
        DeserializationError e = deserializeJson(doc, body);
        if (e) { snap.error = String("json: ") + e.c_str(); return snap; }
        JsonArray execs = doc["executions"].as<JsonArray>();
        snap.running = execs.size();
        for (JsonObject ex : execs) {
            pushExec(ex, Status::Running, true);
            if (snap.recent.size() >= RD_MAX_EXECUTIONS) break;
        }
    }

    // 2) recent executions (any status)
    if (snap.recent.size() < RD_MAX_EXECUTIONS) {
        int slots = RD_MAX_EXECUTIONS - (int)snap.recent.size();
        path = "/project/" + effProj + "/executions?max=" + String(slots);
        int code2 = 0;
        String body2, err2;
        if (httpGet(apiUrl(base, path), tok, code2, body2, err2)) {
            JsonDocument doc;
            if (deserializeJson(doc, body2) == DeserializationError::Ok) {
                JsonArray execs = doc["executions"].as<JsonArray>();
                for (JsonObject ex : execs) {
                    pushExec(ex, Status::Unknown, false);
                    if (snap.recent.size() >= RD_MAX_EXECUTIONS) break;
                }
            }
        }
    }

    // 3) Server-aggregated metrics over RD_METRICS_WINDOW (e.g. last 24h).
    {
        snap.metrics.ok = false;
        snap.metrics.window = String(RD_METRICS_WINDOW);
        String mpath = "/project/" + effProj + "/executions/metrics?recentFilter=" + String(RD_METRICS_WINDOW);
        int mcode = 0;
        String mbody, merr;
        if (httpGet(apiUrl(base, mpath), tok, mcode, mbody, merr)) {
            JsonDocument doc;
            if (deserializeJson(doc, mbody) == DeserializationError::Ok) {
                snap.metrics.total     = doc["total"].as<long>();
                snap.metrics.succeeded = doc["status"]["succeeded"].as<long>();
                snap.metrics.failed    = doc["status"]["failed"].as<long>();
                snap.metrics.aborted   = doc["status"]["aborted"].as<long>();
                // duration.average may be either a number (ms) or a string
                // like "18.234s" / "1m 30s". Handle the common shapes.
                if (doc["duration"]["average"].is<long>()) {
                    snap.metrics.avgDurationMs = doc["duration"]["average"].as<long>();
                } else {
                    const char* s = doc["duration"]["average"].as<const char*>();
                    snap.metrics.avgDurationMs = 0;
                    if (s) {
                        // crude: pull a number, look at suffix
                        double v = atof(s);
                        const char* u = s;
                        while (*u && (*u == '.' || *u == '-' || (*u >= '0' && *u <= '9'))) u++;
                        while (*u == ' ') u++;
                        if      (*u == 'h')                  snap.metrics.avgDurationMs = (long)(v * 3600000);
                        else if (*u == 'm' && u[1] != 's')   snap.metrics.avgDurationMs = (long)(v * 60000);
                        else if (*u == 's')                  snap.metrics.avgDurationMs = (long)(v * 1000);
                        else if (*u == 'm' && u[1] == 's')   snap.metrics.avgDurationMs = (long)v;
                    }
                }
                snap.metrics.ok = true;
            }
        }
    }

    snap.ok = true;
    return snap;
}

bool rundeck::abortExecution(long execId, String& outErr) {
    String base = storage::baseUrl();
    String tok  = storage::token();
    if (base.length() == 0 || tok.length() == 0) {
        outErr = "no config";
        return false;
    }
    int code = 0;
    String body, err;
    String url = apiUrl(base, "/execution/" + String(execId) + "/abort");
    if (!httpGet(url, tok, code, body, err)) {  // Rundeck abort is GET
        outErr = err;
        return false;
    }
    return true;
}
