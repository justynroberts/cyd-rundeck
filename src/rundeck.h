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

    struct Snapshot {
        bool       ok;
        String     error;            // non-empty on failure
        uint32_t   running;          // count of running executions
        uint32_t   succeededRecent;  // last N succeeded
        uint32_t   failedRecent;     // last N failed
        std::vector<Execution> recent;  // bounded to RD_MAX_EXECUTIONS
        String     serverVersion;    // from system/info, cached
    };

    // One-shot calls. All HTTPS, all use the in-NVS base URL + token.
    bool validate(const String& baseUrl, const String& token, String& outErr, String& outVersion);
    Snapshot fetchSnapshot();                       // running + recent
    bool     abortExecution(long execId, String& outErr);

    // Helpers.
    const char* statusText(Status s);
    uint32_t    statusColor(Status s);              // 0xRRGGBB (24-bit)
}
