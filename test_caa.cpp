/*
 * test_caa.cpp — Phase 1 test harness for ZWO CAA rotator
 *
 * Proves connectivity via the ZWO closed-source libCAARotator:
 *   1. Enumerate devices
 *   2. Print firmware + serial number + current position
 *   3. Move to 180 degrees, wait for completion
 *   4. Move back to the original position, wait for completion
 *
 * Build:
 *   g++ -std=c++17 -Wall -O2 -I/usr/local/include \
 *       -o test_caa test_caa.cpp \
 *       -L/usr/local/lib -lCAARotator -lusb-1.0 -lpthread
 *
 * Usage:
 *   ./test_caa [target_angle]
 *   target_angle defaults to 180.0 if not supplied.
 */

#include <CAA_API.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

// ── helpers ──────────────────────────────────────────────────────────────────

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

#define CHECK(call)                                                    \
    do {                                                               \
        CAA_ERROR_CODE _rc = (call);                                   \
        if (_rc != CAA_SUCCESS) {                                      \
            fprintf(stderr, "FAIL  %s  →  %s\n", #call, errStr(_rc)); \
            return 1;                                                  \
        }                                                              \
    } while (0)

// Wait up to timeout_s seconds for motion to stop.  Returns true on success.
static bool waitForStop(int id, int timeout_s = 120)
{
    using namespace std::chrono;
    auto deadline = steady_clock::now() + seconds(timeout_s);
    while (steady_clock::now() < deadline) {
        bool moving = false, byHand = false;
        if (CAAIsMoving(id, &moving, &byHand) != CAA_SUCCESS)
            return false;
        if (!moving)
            return true;
        std::this_thread::sleep_for(milliseconds(200));
        putchar('.');
        fflush(stdout);
    }
    putchar('\n');
    fprintf(stderr, "Timeout waiting for motion to stop.\n");
    return false;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    float targetAngle = 180.0f;
    if (argc >= 2)
        targetAngle = static_cast<float>(atof(argv[1]));

    // SDK version
    printf("SDK version: %s\n", CAAGetSDKVersion());

    // Enumerate
    int numDevices = CAAGetNum();
    printf("Found %d CAA rotator(s)\n", numDevices);
    if (numDevices == 0) {
        fprintf(stderr,
                "No devices found.  Check:\n"
                "  - lsusb shows 03c3:1f20\n"
                "  - udev rule installed and triggered\n"
                "  - ldconfig -p | grep CAA shows the library\n");
        return 1;
    }

    // Enumerate and list all devices; capture the first ID for use below.
    // CAAGetID(index) must be called before CAAGetProperty touches the device,
    // as the library appears to invalidate the index list after that call.
    int firstId = -1;
    for (int i = 0; i < numDevices; i++) {
        int id = -1;
        CHECK(CAAGetID(i, &id));
        if (i == 0) firstId = id;
        CAA_INFO info{};
        CHECK(CAAGetProperty(id, &info));
        printf("  [%d] ID=%-3d  Name=\"%s\"  MaxStep=%d\n",
               i, info.ID, info.Name, info.MaxStep);
    }

    // Open first device (reuse ID captured above — do not call CAAGetID again)
    int id = firstId;
    printf("Opening ID %d ... ", id);
    CHECK(CAAOpen(id));
    printf("OK\n");

    // Firmware
    unsigned char maj = 0, min = 0, bld = 0;
    if (CAAGetFirmwareVersion(id, &maj, &min, &bld) == CAA_SUCCESS)
        printf("Firmware: %u.%u.%u\n", maj, min, bld);
    else
        printf("Firmware: (not available)\n");

    // Serial number
    CAA_SN sn{};
    if (CAAGetSerialNumber(id, &sn) == CAA_SUCCESS) {
        printf("Serial:   ");
        for (int i = 0; i < 8; i++)
            printf("%02X", sn.id[i]);
        printf("\n");
    } else {
        printf("Serial:   (not supported by this firmware)\n");
    }

    // Type
    CAA_TYPE caaType{};
    if (CAAGetType(id, &caaType) == CAA_SUCCESS)
        printf("Type:     %s\n", caaType.type);

    // Temperature
    float temp = 0;
    if (CAAGetTemp(id, &temp) == CAA_SUCCESS)
        printf("Temp:     %.1f °C\n", temp);

    // Current position
    float startPos = 0;
    CHECK(CAAGetDegree(id, &startPos));
    printf("Current position: %.2f deg\n", startPos);

    // ── Move to target ──────────────────────────────────────────────────────
    printf("Moving to %.1f deg ...\n", targetAngle);
    CHECK(CAAMoveTo(id, targetAngle));
    printf("Waiting for motion to complete ");
    if (!waitForStop(id)) {
        CAAStop(id);
        CAAClose(id);
        return 1;
    }

    float posAfter = 0;
    CHECK(CAAGetDegree(id, &posAfter));
    printf("\nPosition after move: %.2f deg\n", posAfter);

    // ── Move back to start ──────────────────────────────────────────────────
    printf("Moving back to original position (%.2f deg) ...\n", startPos);
    CHECK(CAAMoveTo(id, startPos));
    printf("Waiting for motion to complete ");
    if (!waitForStop(id)) {
        CAAStop(id);
        CAAClose(id);
        return 1;
    }

    float posRestored = 0;
    CHECK(CAAGetDegree(id, &posRestored));
    printf("\nPosition restored: %.2f deg\n", posRestored);

    // ── Done ────────────────────────────────────────────────────────────────
    CAAClose(id);
    printf("Closed.\n");
    printf("\nPhase 1 PASS — ZWO library + device communication confirmed.\n");
    return 0;
}
