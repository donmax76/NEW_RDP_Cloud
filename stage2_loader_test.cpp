// stage2_loader_test.cpp — exercises stage2::Registry with the real cache flow.
//
// Pipeline:
//   1. Copy build\stage2\Stage2Sample.bin -> %TEMP%\pnp_cache\sample.bin
//   2. Teach the registry that "sample_ping" and "sample_echo" map to the
//      "sample" module (via a local cmd_to_module override).
//   3. Dispatch "sample_ping" — registry finds no handler, triggers
//      on-demand load from cache, Stage2Init registers handlers, retry
//      succeeds, prints captured response.
//   4. Dispatch "sample_echo" — handler already registered, runs directly.
//   5. shutdown_all — verifies Stage2Shutdown runs and cache file is deleted.
//
// Note: Stage2Sample registers commands named "stage2_ping"/"stage2_echo",
// and the command-to-module map in stage2_loader.h does not include them.
// So for this test we bypass the prefix map by calling ensure_loaded("sample")
// directly and then dispatching the raw command names the sample registers.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "stage2_loader.h"

static void flush_printf(const char* s) { fputs(s, stdout); fflush(stdout); }
#define LOGF(...) do { char _b[1024]; _snprintf_s(_b, sizeof(_b), _TRUNCATE, __VA_ARGS__); flush_printf(_b); } while(0)

// ── Provide the stage-1-side free functions the loader expects ──────────
namespace stage2 {
    static std::vector<std::string> g_sends;
    static std::vector<std::string> g_logs;
    static std::string              g_token = "dev-token";

    void stage1_ws_send(const char* j) {
        g_sends.push_back(j ? j : "");
        LOGF("  [send] %s\n", j ? j : "");
    }
    void stage1_ws_send_bin(const uint8_t*, size_t n, const char* t) {
        LOGF("  [send_bin] type=%s bytes=%zu\n", t ? t : "", n);
    }
    void stage1_log(int lvl, const char* m) {
        char buf[1024];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[log L%d] %s", lvl, m ? m : "");
        g_logs.push_back(buf);
        LOGF("  %s\n", buf);
    }
    const char* stage1_get_config(const char*) { return nullptr; }
    int  stage1_get_config_int(const char*, int d) { return d; }
    std::string stage1_room_token() { return g_token; }
    void stage1_stop_stream() { /* mock */ }
    void stage1_host_exit(int /*code*/) { /* mock */ }
}

static bool copy_blob_to_cache() {
    // Source: build/stage2/Stage2Sample.bin   Dest: %TEMP%\pnp_cache\sample.bin
    char tmp[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string dst(tmp, n); dst += "pnp_cache\\"; CreateDirectoryA(dst.c_str(), nullptr);
    dst += "sample.bin";

    const char* src = "build\\stage2\\Stage2Sample.bin";
    std::ifstream in(src, std::ios::binary);
    if (!in) { printf("cannot open %s\n", src); return false; }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    LOGF("[pre] copied %s -> %s\n", src, dst.c_str());
    return true;
}

int main() {
    LOGF("[main] start\n");
    if (!copy_blob_to_cache()) return 1;
    LOGF("[main] cache copied\n");

    auto& reg = stage2::Registry::inst();
    LOGF("[main] got registry\n");

    LOGF("\n[1] ensure_loaded('sample')\n");
    bool ok_load = reg.ensure_loaded("sample");
    LOGF("    ensure_loaded returned %d\n", ok_load ? 1 : 0);
    if (!ok_load) return 1;

    // Stage2Sample registered "stage2_ping" and "stage2_echo" during load.
    LOGF("\n[2] dispatch('stage2_ping', '{}')\n");
    bool ok1 = reg.dispatch("stage2_ping", "{}");
    LOGF("    dispatch returned %s\n", ok1 ? "true" : "false");

    LOGF("\n[3] dispatch('stage2_echo', '{\"k\":7}')\n");
    bool ok2 = reg.dispatch("stage2_echo", "{\"k\":7}");
    LOGF("    dispatch returned %s\n", ok2 ? "true" : "false");

    LOGF("\n[4] dispatch('nonexistent_cmd', '{}')\n");
    bool ok3 = reg.dispatch("nonexistent_cmd", "{}");
    LOGF("    dispatch returned %s (expected false)\n", ok3 ? "true" : "false");

    LOGF("\n[5] shutdown_all\n");
    reg.shutdown_all();

    // Verify the cache file was deleted.
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    std::string cache_file = std::string(tmp) + "pnp_cache\\sample.bin";
    DWORD attr = GetFileAttributesA(cache_file.c_str());
    bool deleted = (attr == INVALID_FILE_ATTRIBUTES);
    LOGF("    cache file %s: %s\n", cache_file.c_str(), deleted ? "deleted" : "STILL PRESENT");

    bool pass = ok1 && ok2 && !ok3 && deleted &&
                stage2::g_sends.size() >= 2;

    LOGF("\n── SUMMARY ─────────────────────────────\n");
    LOGF("  dispatches OK:       %s (ping=%d, echo=%d, nonexistent=%d)\n",
           (ok1 && ok2 && !ok3) ? "yes" : "no", (int)ok1, (int)ok2, (int)ok3);
    LOGF("  sends captured:      %zu\n", stage2::g_sends.size());
    LOGF("  cache deleted:       %s\n", deleted ? "yes" : "NO");
    LOGF("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
