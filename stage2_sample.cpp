// ═══════════════════════════════════════════════════════════════════════
// stage2_sample.cpp — minimal stage-2 module used as a smoke test for the
// reflective-load / ABI pipeline.
//
// Registers two trivial commands:
//   "stage2_ping"  — responds with {"ok":1,"from":"stage2"}
//   "stage2_echo"  — echoes back the json_args verbatim
//
// When the full architecture is in place, real modules (screenshot, audio,
// stream, filemgr, procmgr, defender) follow this exact skeleton.
// ═══════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstring>

#include "stage2_abi.h"

static Stage2HostCtx* g_host = nullptr;

// ── Command: "stage2_ping" ─────────────────────────────────────────────
static void cmd_ping(const char* /*json_args*/, void* /*user_ctx*/) {
    if (!g_host) return;
    g_host->send("{\"ok\":1,\"from\":\"stage2_sample\",\"cmd\":\"stage2_ping\"}");
    g_host->log(1, "stage2_sample: ping handled");
}

// ── Command: "stage2_echo" ─────────────────────────────────────────────
static void cmd_echo(const char* json_args, void* /*user_ctx*/) {
    if (!g_host) return;
    std::string resp = "{\"ok\":1,\"from\":\"stage2_sample\",\"echo\":";
    resp += (json_args && *json_args) ? json_args : "null";
    resp += "}";
    g_host->send(resp.c_str());
}

// ── Entry point ────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) int Stage2Init(Stage2HostCtx* host) {
    if (!host || host->abi_version != STAGE2_ABI_VERSION) return 1;
    g_host = host;

    host->log(1, "stage2_sample: Stage2Init called");

    host->register_cmd("stage2_ping", cmd_ping, nullptr);
    host->register_cmd("stage2_echo", cmd_echo, nullptr);

    host->log(1, "stage2_sample: 2 commands registered");
    return 0;
}

// ── Optional shutdown ──────────────────────────────────────────────────
extern "C" __declspec(dllexport) void Stage2Shutdown(void) {
    if (g_host) g_host->log(1, "stage2_sample: Stage2Shutdown called");
    g_host = nullptr;
}

// ── DllMain: keep it minimal; real init is in Stage2Init ──────────────
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(nullptr);
    return TRUE;
}
