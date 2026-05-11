#pragma once
#include <Arduino.h>
#include <vector>

namespace rundeck {

    enum class Status : uint8_t {
        Unknown,
        Running,
        Succeeded,
        Failed,
        Aborted,
        TimedOut,
        FailedWithRetry,
        Scheduled,
    };

    struct Execution {
        long       id;            // numeric execution id
        Status     status;
        String     project;
        String     jobName;       // empty for adhoc
        String     user;
        uint32_t   startedEpoch;  // seconds (best-effort; 0 if unparsed)
        uint32_t   endedEpoch;    // seconds; 0 if running
        long       durationSec;   // computed, -1 if running
    };

    struct Metrics {
        bool   ok;             // true if the metrics fetch succeeded
        String window;         // e.g. "24h" (what was requested)
        long   total;          // total executions in window
        long   succeeded;
        long   failed;
        long   aborted;
        long   avgDurationMs;  // average duration of finished execs
    };

    struct Snapshot {
        bool       ok;
        String     error;            // non-empty on failure
        uint32_t   running;          // count of running executions
        uint32_t   succeededRecent;  // last N succeeded (rows-window)
        uint32_t   failedRecent;     // last N failed (rows-window)
        std::vector<Execution> recent;  // bounded to RD_MAX_EXECUTIONS
        String     serverVersion;    // from system/info, cached
        Metrics    metrics;          // server-aggregated, time-windowed
    };

    // One-shot calls. All HTTPS, all use the in-NVS base URL + token.
    bool validate(const String& baseUrl, const String& token, String& outErr, String& outVersion);
    Snapshot fetchSnapshot();                       // running + recent
    bool     abortExecution(long execId, String& outErr);

    // Helpers.
    const char* statusText(Status s);
    uint32_t    statusColor(Status s);              // 0xRRGGBB (24-bit)
}
