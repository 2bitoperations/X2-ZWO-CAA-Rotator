/*
 * test_caa_probe.cpp — Phase 2 protocol probe
 *
 * Exercises every CAA_API call not covered in test_caa.cpp, using small
 * safe movements.  Designed to run under LD_PRELOAD=./intercept.so so
 * every HID feature report is captured.
 *
 * Build:
 *   g++ -std=c++17 -Wall -O2 -I/usr/local/include \
 *       -o test_caa_probe test_caa_probe.cpp \
 *       -L/usr/local/lib -lCAARotator -lusb-1.0 -ludev -lpthread
 */

#include <CAA_API.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

static const char *errStr(CAA_ERROR_CODE e)
{
    switch (e) {
    case CAA_SUCCESS:             return "CAA_SUCCESS";
    case CAA_ERROR_INVALID_INDEX: return "CAA_ERROR_INVALID_INDEX";
    case CAA_ERROR_INVALID_ID:    return "CAA_ERROR_INVALID_ID";
    case CAA_ERROR_INVALID_VALUE: return "CAA_ERROR_INVALID_VALUE";
    case CAA_ERROR_REMOVED:       return "CAA_ERROR_REMOVED";
    case CAA_ERROR_MOVING:        return "CAA_ERROR_MOVING";
    case CAA_ERROR_ERROR_STATE:   return "CAA_ERROR_ERROR_STATE";
    case CAA_ERROR_GENERAL_ERROR: return "CAA_ERROR_GENERAL_ERROR";
    case CAA_ERROR_NOT_SUPPORTED: return "CAA_ERROR_NOT_SUPPORTED";
    case CAA_ERROR_CLOSED:        return "CAA_ERROR_CLOSED";
    case CAA_ERROR_OUT_RANGE:     return "CAA_ERROR_OUT_RANGE";
    case CAA_ERROR_OVER_LIMIT:    return "CAA_ERROR_OVER_LIMIT";
    case CAA_ERROR_STALL:         return "CAA_ERROR_STALL";
    case CAA_ERROR_TIMEOUT:       return "CAA_ERROR_TIMEOUT";
    case CAA_ERROR_INVALID_LENGTH:return "CAA_ERROR_INVALID_LENGTH";
    default:                      return "CAA_ERROR_UNKNOWN";
    }
}

#define CHECK(call)                                                         \
    do {                                                                    \
        CAA_ERROR_CODE _rc = (call);                                        \
        if (_rc != CAA_SUCCESS) {                                           \
            fprintf(stderr, "  FAIL  %-40s  %s\n", #call, errStr(_rc));    \
            return false;                                                   \
        } else {                                                            \
            printf("  OK    %s\n", #call);                                  \
        }                                                                   \
    } while (0)

#define TRY(call)                                                           \
    do {                                                                    \
        CAA_ERROR_CODE _rc = (call);                                        \
        printf("  %-6s %-40s\n", (_rc==CAA_SUCCESS?"OK":"FAIL"), #call);   \
        if (_rc != CAA_SUCCESS)                                             \
            printf("         → %s\n", errStr(_rc));                        \
    } while (0)

static void sep(const char *label) {
    printf("\n── %s ──\n", label);
}

static bool waitForStop(int id, int timeout_s = 30)
{
    using namespace std::chrono;
    auto deadline = steady_clock::now() + seconds(timeout_s);
    while (steady_clock::now() < deadline) {
        bool moving = false, byHand = false;
        if (CAAIsMoving(id, &moving, &byHand) != CAA_SUCCESS) return false;
        if (!moving) return true;
        std::this_thread::sleep_for(milliseconds(200));
        putchar('.');
        fflush(stdout);
    }
    printf("\n");
    fprintf(stderr, "  Timeout waiting for motion stop\n");
    return false;
}

static bool runProbe(int id, float startPos)
{
    float v;
    bool bv;

    // ── 1. CAAGetBeep / CAASetBeep ────────────────────────────────────────
    sep("CAAGetBeep / CAASetBeep");
    bool beepOn = false;
    CHECK(CAAGetBeep(id, &beepOn));
    printf("         beep = %s\n", beepOn ? "true" : "false");

    // toggle beep off then restore
    CHECK(CAASetBeep(id, false));
    CHECK(CAAGetBeep(id, &bv));
    printf("         beep after SetBeep(false) = %s\n", bv ? "true" : "false");
    CHECK(CAASetBeep(id, beepOn));   // restore
    CHECK(CAAGetBeep(id, &bv));
    printf("         beep restored = %s\n", bv ? "true" : "false");

    // ── 2. CAAGetReverse / CAASetReverse ─────────────────────────────────
    sep("CAAGetReverse / CAASetReverse");
    bool revOn = false;
    CHECK(CAAGetReverse(id, &revOn));
    printf("         reverse = %s\n", revOn ? "true" : "false");

    CHECK(CAASetReverse(id, true));
    CHECK(CAAGetReverse(id, &bv));
    printf("         reverse after SetReverse(true) = %s\n", bv ? "true" : "false");
    CHECK(CAASetReverse(id, false));   // restore to forward
    CHECK(CAAGetReverse(id, &bv));
    printf("         reverse restored = %s\n", bv ? "true" : "false");

    // ── 3. CAAGetMaxDegree / CAASetMaxDegree ─────────────────────────────
    sep("CAAGetMaxDegree / CAASetMaxDegree");
    float origMax = 0;
    CHECK(CAAGetMaxDegree(id, &origMax));
    printf("         max_degree = %.2f\n", origMax);

    // set to a slightly different value and read back
    float newMax = (origMax > 180.0f) ? 180.0f : origMax;
    printf("         calling CAASetMaxDegree(%.2f)\n", newMax);
    CHECK(CAASetMaxDegree(id, newMax));
    CHECK(CAAGetMaxDegree(id, &v));
    printf("         max_degree after set = %.2f\n", v);

    // restore
    CHECK(CAASetMaxDegree(id, origMax));
    CHECK(CAAGetMaxDegree(id, &v));
    printf("         max_degree restored = %.2f\n", v);

    // ── 4. CAAGetTemp ─────────────────────────────────────────────────────
    // Note: returns CAA_ERROR_GENERAL_ERROR when sensor is unavailable (documented)
    sep("CAAGetTemp");
    float temp = 0;
    TRY(CAAGetTemp(id, &temp));
    printf("         temp = %.1f °C\n", temp);

    // ── 6. CAAGetDegree (current position) ────────────────────────────────
    sep("CAAGetDegree");
    CHECK(CAAGetDegree(id, &v));
    printf("         current_position = %.4f°\n", v);

    // ── 7. CAAMove (relative) ─────────────────────────────────────────────
    sep("CAAMove (relative +3.0 deg)");
    printf("         current pos = %.4f° — moving +3.0°\n", v);
    CHECK(CAAMove(id, 3.0f));
    printf("         waiting for stop ");
    if (!waitForStop(id)) return false;
    float posAfterRelMove = 0;
    CHECK(CAAGetDegree(id, &posAfterRelMove));
    printf("\n         position after +3.0° relative move = %.4f°\n", posAfterRelMove);

    // ── 8. CAAIsMoving ─────────────────────────────────────────────────────
    sep("CAAIsMoving (while idle)");
    bool moving = false, byHand = false;
    CHECK(CAAIsMoving(id, &moving, &byHand));
    printf("         moving=%s  byHand=%s\n",
           moving ? "true" : "false", byHand ? "true" : "false");

    // ── 9. CAAStop (issue stop during a move) ─────────────────────────────
    sep("CAAStop (stop mid-move)");
    // Start a 5° move, then immediately stop it
    float preStop = 0;
    CHECK(CAAGetDegree(id, &preStop));
    printf("         current pos = %.4f° — starting CAAMoveTo(%.2f°)\n",
           preStop, preStop + 5.0f);
    CHECK(CAAMoveTo(id, preStop + 5.0f));
    // Brief pause to let motion begin
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(CAAStop(id));
    printf("         CAAStop issued — waiting for full stop ");
    waitForStop(id);  // don't fail if already stopped
    float posAfterStop = 0;
    CHECK(CAAGetDegree(id, &posAfterStop));
    printf("\n         position after stop = %.4f°\n", posAfterStop);

    // ── 10. CAAMoveToMechanical ────────────────────────────────────────────
    sep("CAAMoveToMechanical");
    float preM = 0;
    CHECK(CAAGetDegree(id, &preM));
    printf("         current pos = %.4f° — calling CAAMoveToMechanical(%.2f°)\n",
           preM, preM);
    // Move to mechanical position = current logical position (no-op motion,
    // but probes the command packet format)
    CHECK(CAAMoveToMechanical(id, preM));
    printf("         waiting for stop ");
    waitForStop(id);
    float posAfterMech = 0;
    CHECK(CAAGetDegree(id, &posAfterMech));
    printf("\n         position after MoveToMechanical = %.4f°\n", posAfterMech);

    // ── 11. CAACurDegree (set current position label) ─────────────────────
    sep("CAACurDegree (set position label)");
    float curPos = 0;
    CHECK(CAAGetDegree(id, &curPos));
    printf("         position before = %.4f°\n", curPos);
    // Re-label current position as itself — should be a no-op physically
    CHECK(CAACurDegree(id, curPos));
    CHECK(CAAGetDegree(id, &v));
    printf("         position after CAACurDegree(%.4f°) = %.4f°\n", curPos, v);

    // ── 12. Return to original start position ─────────────────────────────
    sep("Return to start position");
    printf("         moving back to start: %.4f°\n", startPos);
    CHECK(CAAMoveTo(id, startPos));
    printf("         waiting for stop ");
    if (!waitForStop(id)) return false;
    float finalPos = 0;
    CHECK(CAAGetDegree(id, &finalPos));
    printf("\n         final position = %.4f°\n", finalPos);

    return true;
}

int main()
{
    printf("=== CAA Protocol Probe (Phase 2) ===\n");
    printf("SDK: %s\n\n", CAAGetSDKVersion());

    int numDevices = CAAGetNum();
    printf("Found %d device(s)\n", numDevices);
    if (numDevices == 0) { fprintf(stderr, "No device.\n"); return 1; }

    int firstId = -1;
    for (int i = 0; i < numDevices; i++) {
        int id = -1;
        if (CAAGetID(i, &id) != CAA_SUCCESS) continue;
        if (i == 0) firstId = id;
        CAA_INFO info{};
        if (CAAGetProperty(id, &info) == CAA_SUCCESS)
            printf("  [%d] ID=%-3d  Name=\"%s\"  MaxStep=%d\n",
                   i, info.ID, info.Name, info.MaxStep);
    }

    int id = firstId;
    printf("\nOpening ID %d ... ", id);
    if (CAAOpen(id) != CAA_SUCCESS) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("OK\n");

    float startPos = 0;
    CAAGetDegree(id, &startPos);
    printf("Start position: %.4f°\n", startPos);

    bool ok = runProbe(id, startPos);

    CAAClose(id);
    printf("\n%s\n", ok ? "Phase 2 probe COMPLETE." : "Phase 2 probe INCOMPLETE (see errors above).");
    return ok ? 0 : 1;
}
