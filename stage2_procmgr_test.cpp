// stage2_procmgr_test.cpp — integration test for the procmgr stage-2 module.
//
// Tests exercise each handler through the real Registry dispatch path with
// benign effects on the test system:
//   * term_exec: run `echo hello` via cmd.exe and verify captured stdout
//   * reg_create_key under HKCU\Software\Stage2ProcmgrTest
//   * reg_set_value (REG_SZ + REG_DWORD)
//   * reg_delete_value
//   * reg_delete_key (cleans up what we created)
//   * proc_kill / proc_launch / svc_control: argument-validation only
//       (we don't kill real processes or start real services)
//
// Cleanup: deletes the test registry key on every run, even if tests fail.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "stage2_loader.h"

// ── Stage-1 side bridges (mocked) ───────────────────────────────────────
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
}

static bool copy_blob_to_cache() {
    char tmp[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string dst(tmp, n); dst += "pnp_cache\\"; CreateDirectoryA(dst.c_str(), nullptr);
    dst += "procmgr.bin";
    const char* src = "build\\stage2\\procmgr.bin";
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

// Helper: direct registry query (bypass the module) so we can verify
// whether a test actually wrote the key/value.
static bool reg_value_exists(HKEY root, const char* subpath, const char* name) {
    HKEY hKey;
    if (RegOpenKeyExA(root, subpath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    LONG rc = RegQueryValueExA(hKey, name, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

static void force_clean_test_key() {
    HKEY hSub;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Stage2ProcmgrTest",
                      0, KEY_READ, &hSub) == ERROR_SUCCESS) {
        RegCloseKey(hSub);
        RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\Stage2ProcmgrTest");
    }
}

int main() {
    if (!copy_blob_to_cache()) return 1;
    force_clean_test_key();

    printf("\n[LOAD] ensure_loaded('procmgr')\n"); fflush(stdout);
    if (!stage2::Registry::inst().ensure_loaded("procmgr")) {
        printf("FAIL: ensure_loaded\n"); return 1;
    }

    int fails = 0;
    auto& reg = stage2::Registry::inst();

    // ── term_exec ──────────────────────────────────────────────────────
    printf("\n[T1] term_exec 'echo stage2_procmgr_test_ok'\n"); fflush(stdout);
    {
        reg.dispatch("term_exec",
            R"({"id":"1","line":"echo stage2_procmgr_test_ok"})");
        bool resp_ok  = last_send_has("\"ok\":true");
        bool echo_ok  = last_send_has("stage2_procmgr_test_ok");
        printf("  resp=%s echo=%s\n", resp_ok?"yes":"NO", echo_ok?"yes":"NO");
        if (!(resp_ok && echo_ok)) ++fails;
    }

    // ── reg_create_key ─────────────────────────────────────────────────
    printf("\n[T2] reg_create_key HKCU\\Software\\Stage2ProcmgrTest\n"); fflush(stdout);
    {
        reg.dispatch("reg_create_key",
            R"({"id":"2","path":"HKCU\\Software\\Stage2ProcmgrTest"})");
        HKEY hSub;
        bool disk_ok = (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Stage2ProcmgrTest",
                                      0, KEY_READ, &hSub) == ERROR_SUCCESS);
        if (disk_ok) RegCloseKey(hSub);
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("created");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── reg_set_value REG_SZ ───────────────────────────────────────────
    printf("\n[T3] reg_set_value REG_SZ greeting=hello\n"); fflush(stdout);
    {
        reg.dispatch("reg_set_value",
            R"({"id":"3","path":"HKCU\\Software\\Stage2ProcmgrTest","name":"greeting","type":"REG_SZ","data":"hello"})");
        bool disk_ok = reg_value_exists(HKEY_CURRENT_USER,
                                        "Software\\Stage2ProcmgrTest", "greeting");
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("saved");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── reg_set_value REG_DWORD ────────────────────────────────────────
    printf("\n[T4] reg_set_value REG_DWORD counter=42\n"); fflush(stdout);
    {
        reg.dispatch("reg_set_value",
            R"({"id":"4","path":"HKCU\\Software\\Stage2ProcmgrTest","name":"counter","type":"REG_DWORD","data":"42"})");
        HKEY hSub;
        bool disk_ok = false;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Stage2ProcmgrTest",
                          0, KEY_QUERY_VALUE, &hSub) == ERROR_SUCCESS) {
            DWORD val = 0, sz = sizeof(val), type = 0;
            if (RegQueryValueExA(hSub, "counter", nullptr, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS)
                disk_ok = (type == REG_DWORD && val == 42);
            RegCloseKey(hSub);
        }
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("saved");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── reg_delete_value ───────────────────────────────────────────────
    printf("\n[T5] reg_delete_value greeting\n"); fflush(stdout);
    {
        reg.dispatch("reg_delete_value",
            R"({"id":"5","path":"HKCU\\Software\\Stage2ProcmgrTest","name":"greeting"})");
        bool disk_ok = !reg_value_exists(HKEY_CURRENT_USER,
                                         "Software\\Stage2ProcmgrTest", "greeting");
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("deleted");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── reg_delete_key (note: RegDeleteKeyA requires key to be empty first) ─
    printf("\n[T6] reg_delete_key (first delete counter, then key)\n"); fflush(stdout);
    {
        reg.dispatch("reg_delete_value",
            R"({"id":"6a","path":"HKCU\\Software\\Stage2ProcmgrTest","name":"counter"})");
        reg.dispatch("reg_delete_key",
            R"({"id":"6b","path":"HKCU\\Software\\Stage2ProcmgrTest"})");
        HKEY hSub;
        bool disk_ok = (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Stage2ProcmgrTest",
                                      0, KEY_READ, &hSub) != ERROR_SUCCESS);
        if (!disk_ok) RegCloseKey(hSub);
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("deleted");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── proc_kill: invalid pid ─────────────────────────────────────────
    printf("\n[T7] proc_kill missing pid (arg validation)\n"); fflush(stdout);
    {
        reg.dispatch("proc_kill", R"({"id":"7"})");
        bool resp_ok = last_send_has("\"ok\":false") && last_send_has("Missing pid");
        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    // ── proc_launch: empty exe ─────────────────────────────────────────
    printf("\n[T8] proc_launch empty exe (arg validation)\n"); fflush(stdout);
    {
        reg.dispatch("proc_launch", R"({"id":"8"})");
        bool resp_ok = last_send_has("\"ok\":false") && last_send_has("empty exe");
        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    // ── svc_control: missing fields ────────────────────────────────────
    printf("\n[T9] svc_control missing name (arg validation)\n"); fflush(stdout);
    {
        reg.dispatch("svc_control", R"({"id":"9","action":"start"})");
        bool resp_ok = last_send_has("\"ok\":false");
        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    force_clean_test_key();

    printf("\n[shutdown] shutdown_all\n"); fflush(stdout);
    reg.shutdown_all();

    printf("\n── SUMMARY ─── fails=%d of 9 tests\n", fails);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
