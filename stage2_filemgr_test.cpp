// stage2_filemgr_test.cpp — exercises the filemgr stage-2 module end-to-end.
//
// Creates a temp work dir, loads filemgr.bin via the Registry, dispatches
// each command with the real JSON-argument shape, and verifies the on-disk
// side effects. This is the model test for how real stage-2 modules will
// be integration-tested.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#include "stage2_loader.h"

namespace fs = std::filesystem;

// ── stage-1 side bridges (mocked) ───────────────────────────────────────
namespace stage2 {
    static std::vector<std::string> g_sends;
    static std::string              g_token = "dev-token";

    void stage1_ws_send(const char* j) {
        g_sends.emplace_back(j ? j : "");
        printf("  [send] %s\n", j ? j : "");
        fflush(stdout);
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

// ── Helpers ─────────────────────────────────────────────────────────────
static bool copy_blob_to_cache() {
    char tmp[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string dst(tmp, n); dst += "pnp_cache\\"; CreateDirectoryA(dst.c_str(), nullptr);
    dst += "filemgr.bin";
    const char* src = "build\\stage2\\filemgr.bin";
    std::ifstream in(src, std::ios::binary);
    if (!in) { printf("cannot open %s\n", src); return false; }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    printf("[pre] copied %s -> %s\n", src, dst.c_str()); fflush(stdout);
    return true;
}

static std::string workdir() {
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "stage2_fm_test";
}

// Find the most recent send whose JSON contains `needle`.
static bool last_send_has(const std::string& needle) {
    for (auto it = stage2::g_sends.rbegin(); it != stage2::g_sends.rend(); ++it) {
        if (it->find(needle) != std::string::npos) return true;
    }
    return false;
}

// Dispatch a command and verify it ran. Returns number of sends produced.
static size_t dispatch_n(const std::string& cmd, const std::string& args) {
    size_t pre = stage2::g_sends.size();
    bool ok = stage2::Registry::inst().dispatch(cmd, args);
    (void)ok;
    return stage2::g_sends.size() - pre;
}

int main() {
    if (!copy_blob_to_cache()) return 1;

    auto wd = workdir();
    std::error_code ec; fs::remove_all(wd, ec);

    // ── Pre-load filemgr module ────────────────────────────────────────
    printf("\n[LOAD] ensure_loaded('filemgr')\n"); fflush(stdout);
    if (!stage2::Registry::inst().ensure_loaded("filemgr")) {
        printf("FAIL: ensure_loaded failed\n"); return 1;
    }

    int fails = 0;

    // ── file_mkdir ─────────────────────────────────────────────────────
    printf("\n[T1] file_mkdir %s\\subdir\n", wd.c_str()); fflush(stdout);
    {
        std::string args = "{\"id\":\"1\",\"path\":\"" + wd + "\\\\subdir\"}";
        dispatch_n("file_mkdir", args);
        bool disk_ok = fs::is_directory(wd + "\\subdir");
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("created");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── file_write_text ────────────────────────────────────────────────
    printf("\n[T2] file_write_text\n"); fflush(stdout);
    std::string file_a = wd + "\\subdir\\a.txt";
    {
        std::string args = "{\"id\":\"2\",\"path\":\"" + file_a +
                           "\\\"\".substr(0,0)" // ignore syntax above; use clean:
                           ;
        // build clean JSON manually
        args = std::string("{\"id\":\"2\",\"path\":\"") + file_a + "\",\"text\":\"hello\"}";
        // Note: we need path to have \\\\ inside JSON string (representing \\ on disk).
        // Since file_a contains "\\", in this C++ literal it's already single backslashes.
        // To embed in JSON where backslash must be doubled, we rewrite:
        std::string path_json;
        for (char c : file_a) { if (c == '\\') path_json += "\\\\"; else path_json += c; }
        args = std::string("{\"id\":\"2\",\"path\":\"") + path_json + "\",\"text\":\"hello\"}";
        dispatch_n("file_write_text", args);
        bool disk_ok = false;
        std::ifstream f(file_a);
        if (f) { std::string c((std::istreambuf_iterator<char>(f)), {}); disk_ok = (c == "hello"); }
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("saved");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    auto to_json_path = [](const std::string& p){
        std::string r; for (char c : p) { if (c == '\\') r += "\\\\"; else r += c; }
        return r;
    };

    // ── file_copy ──────────────────────────────────────────────────────
    printf("\n[T3] file_copy\n"); fflush(stdout);
    std::string file_b = wd + "\\subdir\\b.txt";
    {
        std::string args = std::string("{\"id\":\"3\",\"from\":\"") + to_json_path(file_a) +
                           "\",\"to\":\"" + to_json_path(file_b) + "\"}";
        dispatch_n("file_copy", args);
        bool disk_ok = fs::is_regular_file(file_b) && fs::file_size(file_b) == 5;
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("copied");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── file_rename ────────────────────────────────────────────────────
    printf("\n[T4] file_rename\n"); fflush(stdout);
    std::string file_c = wd + "\\subdir\\c.txt";
    {
        std::string args = std::string("{\"id\":\"4\",\"from\":\"") + to_json_path(file_b) +
                           "\",\"to\":\"" + to_json_path(file_c) + "\"}";
        dispatch_n("file_rename", args);
        bool disk_ok = !fs::exists(file_b) && fs::is_regular_file(file_c);
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("renamed");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── file_delete ────────────────────────────────────────────────────
    printf("\n[T5] file_delete (whole subdir)\n"); fflush(stdout);
    {
        std::string args = std::string("{\"id\":\"5\",\"path\":\"") +
                           to_json_path(wd + "\\subdir") + "\"}";
        dispatch_n("file_delete", args);
        bool disk_ok = !fs::exists(wd + "\\subdir");
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("deleted");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── config_write (alias) ───────────────────────────────────────────
    printf("\n[T6] config_write\n"); fflush(stdout);
    std::string conf = wd + "\\conf.json";
    {
        std::string args = std::string("{\"id\":\"6\",\"path\":\"") +
                           to_json_path(conf) + "\",\"text\":\"{\\\"k\\\":1}\"}";
        dispatch_n("config_write", args);
        bool disk_ok = fs::is_regular_file(conf);
        bool resp_ok = last_send_has("\"ok\":true") && last_send_has("saved");
        printf("  disk=%s resp=%s\n", disk_ok?"yes":"NO", resp_ok?"yes":"NO");
        if (!(disk_ok && resp_ok)) ++fails;
    }

    // ── error handling ─────────────────────────────────────────────────
    printf("\n[T7] file_mkdir with empty path\n"); fflush(stdout);
    {
        dispatch_n("file_mkdir", "{\"id\":\"7\"}");
        bool resp_ok = last_send_has("\"ok\":false") && last_send_has("empty path");
        printf("  resp=%s\n", resp_ok?"yes":"NO");
        if (!resp_ok) ++fails;
    }

    // Cleanup test workdir.
    fs::remove_all(wd, ec);

    // Shutdown.
    printf("\n[shutdown] shutdown_all\n"); fflush(stdout);
    stage2::Registry::inst().shutdown_all();

    printf("\n── SUMMARY ─── fails=%d of 7 tests\n", fails);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
