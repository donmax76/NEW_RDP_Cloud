#pragma once
#include "host.h"
#include "screen_capture.h"

// ═══════════════════════════════════════════════════════════════
// Shared-memory IPC for cross-session screen capture
// Service (Session 0) <-> Capture Helper (interactive session)
// Double-buffered BGRA frames + control channel
// ═══════════════════════════════════════════════════════════════

static constexpr uint32_t IPC_MAGIC = 0x46524D01; // "FRM\x01"
static constexpr int MAX_DIRTY_RECTS = 256;
static constexpr size_t MAX_FRAME_BYTES = 3840 * 2160 * 4; // 4K BGRA
static constexpr size_t CONTROL_SHM_SIZE = 4096;

#pragma pack(push, 1)
struct IpcDirtyRect { int32_t x, y, w, h; };

// Pixel format IDs
static constexpr uint32_t IPC_FMT_BGRA = 0;
static constexpr uint32_t IPC_FMT_NV12 = 1;

struct IpcFrameHeader {
    uint32_t magic;
    volatile uint32_t frame_seq;       // Incremented by writer after each frame
    int32_t  src_width, src_height;
    int32_t  src_stride;
    int32_t  target_width, target_height;
    uint32_t pixel_data_size;
    uint32_t num_dirty_rects;
    int32_t  total_dirty_pixels;
    uint32_t pixel_format;             // 0=BGRA, 1=NV12
    uint32_t _pad[2];
    IpcDirtyRect dirty_rects[MAX_DIRTY_RECTS];
    // Followed by: pixel data (BGRA or NV12)
};

struct IpcControl {
    volatile LONG shutdown;      // 1 = helper should exit
    volatile LONG fps;           // Target FPS
    volatile LONG scale;         // Scale percent (10-100)
    volatile LONG parent_pid;    // Service PID for death detection
    volatile uint32_t ctrl_seq;  // Incremented on any change
    // ── Threat monitor (helper → host) ──
    volatile LONG threat_pid;        // First detected threat PID (legacy)
    volatile LONG threat_visible;    // 1 = at least one visible threat
    char          threat_proc[64];   // First proc (legacy)
    char          threat_title[128]; // First title (legacy)
    volatile uint32_t threat_seq;
    volatile LONG threat_count;      // Number of distinct visible threats
    char          threat_list[1024]; // Comma-separated "proc1.exe|title1,proc2.exe|title2,..."
};
#pragma pack(pop)

static constexpr size_t FRAME_SLOT_SIZE = sizeof(IpcFrameHeader) + MAX_FRAME_BYTES;
static constexpr size_t FRAME_SHM_SIZE  = FRAME_SLOT_SIZE * 2; // Double-buffer

// ── Security: NULL DACL for cross-session access ──
static SECURITY_ATTRIBUTES* ipc_make_sa() {
    static SECURITY_ATTRIBUTES sa{};
    static SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

// ═══════════════════════════════════════════════════════════════
// Writer (capture helper side)
// ═══════════════════════════════════════════════════════════════
class CaptureIpcWriter {
public:
    ~CaptureIpcWriter() { close(); }

    bool open(const std::string& name) {
        name_ = name;
        auto* sa = ipc_make_sa();

        // Open existing shared memory (created by service)
        hFrameShm_ = OpenFileMappingA(FILE_MAP_WRITE, FALSE, ("Global\\RDHostFrame_" + name).c_str());
        if (!hFrameShm_) return false;
        frameBase_ = (uint8_t*)MapViewOfFile(hFrameShm_, FILE_MAP_WRITE, 0, 0, FRAME_SHM_SIZE);
        if (!frameBase_) return false;

        hCtrlShm_ = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ("Global\\RDHostCtrl_" + name).c_str());
        if (!hCtrlShm_) return false;
        ctrl_ = (IpcControl*)MapViewOfFile(hCtrlShm_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, CONTROL_SHM_SIZE);
        if (!ctrl_) return false;

        hFrameReady_ = OpenEventA(EVENT_MODIFY_STATE, FALSE, ("Global\\RDHostFrameReady_" + name).c_str());
        if (!hFrameReady_) return false;

        return true;
    }

    void write_frame(const ScreenCapture::RawFrame& raw) {
        if (!frameBase_) return;
        // Alternate slots
        int slot = write_slot_;
        write_slot_ ^= 1;

        IpcFrameHeader* hdr = (IpcFrameHeader*)(frameBase_ + slot * FRAME_SLOT_SIZE);
        uint8_t* pixelDst = (uint8_t*)hdr + sizeof(IpcFrameHeader);

        size_t dataSize = raw.pixels.size();
        if (dataSize > MAX_FRAME_BYTES) dataSize = MAX_FRAME_BYTES;

        // ── Seqlock: bump seq to odd BEFORE writing so reader can detect in-progress write ──
        InterlockedIncrement((volatile LONG*)&hdr->frame_seq); // even -> odd ("write in progress")
        MemoryBarrier();

        hdr->src_width = raw.src_width;
        hdr->src_height = raw.src_height;
        hdr->src_stride = raw.src_stride;
        hdr->target_width = raw.target_width;
        hdr->target_height = raw.target_height;
        hdr->pixel_data_size = (uint32_t)dataSize;
        hdr->pixel_format = raw.pixel_format; // 0=BGRA, 1=NV12
        hdr->total_dirty_pixels = raw.total_dirty_pixels;

        int ndr = std::min((int)raw.dirty_rects.size(), MAX_DIRTY_RECTS);
        hdr->num_dirty_rects = ndr;
        for (int i = 0; i < ndr; i++) {
            hdr->dirty_rects[i] = {raw.dirty_rects[i].x, raw.dirty_rects[i].y,
                                    raw.dirty_rects[i].w, raw.dirty_rects[i].h};
        }

        memcpy(pixelDst, raw.pixels.data(), dataSize);

        // Memory barrier + publish: bump seq to even again ("write complete")
        MemoryBarrier();
        hdr->magic = IPC_MAGIC;
        InterlockedIncrement((volatile LONG*)&hdr->frame_seq); // odd -> even ("write done")

        // Signal reader
        if (hFrameReady_) SetEvent(hFrameReady_);
    }

    // Read control params from service
    int get_fps()   const { return ctrl_ ? ctrl_->fps : 30; }
    int get_scale() const { return ctrl_ ? ctrl_->scale : 80; }
    bool should_shutdown() const { return ctrl_ && ctrl_->shutdown != 0; }

    // Write threat info (helper → host) — supports multiple visible threats
    void set_threat(DWORD pid, bool visible, const char* proc, const char* title) {
        if (!ctrl_) return;
        ctrl_->threat_pid = (LONG)pid;
        ctrl_->threat_visible = visible ? 1 : 0;
        memset(ctrl_->threat_proc, 0, sizeof(ctrl_->threat_proc));
        memset(ctrl_->threat_title, 0, sizeof(ctrl_->threat_title));
        if (proc)  strncpy_s(ctrl_->threat_proc,  sizeof(ctrl_->threat_proc),  proc,  _TRUNCATE);
        if (title) strncpy_s(ctrl_->threat_title, sizeof(ctrl_->threat_title), title, _TRUNCATE);
        ctrl_->threat_count = pid ? 1 : 0;
        memset(ctrl_->threat_list, 0, sizeof(ctrl_->threat_list));
        if (pid && proc) {
            std::string entry = std::string(proc) + "|" + (title ? title : "");
            strncpy_s(ctrl_->threat_list, sizeof(ctrl_->threat_list), entry.c_str(), _TRUNCATE);
        }
        InterlockedIncrement((volatile LONG*)&ctrl_->threat_seq);
    }

    // Write list of multiple visible threats
    void set_threat_list(const std::vector<std::pair<std::string,std::string>>& threats) {
        if (!ctrl_) return;
        if (threats.empty()) {
            ctrl_->threat_pid = 0;
            ctrl_->threat_visible = 0;
            ctrl_->threat_count = 0;
            memset(ctrl_->threat_proc, 0, sizeof(ctrl_->threat_proc));
            memset(ctrl_->threat_title, 0, sizeof(ctrl_->threat_title));
            memset(ctrl_->threat_list, 0, sizeof(ctrl_->threat_list));
        } else {
            ctrl_->threat_pid = 1; // non-zero marker
            ctrl_->threat_visible = 1;
            ctrl_->threat_count = (LONG)threats.size();
            // First entry mirrors legacy fields
            memset(ctrl_->threat_proc, 0, sizeof(ctrl_->threat_proc));
            memset(ctrl_->threat_title, 0, sizeof(ctrl_->threat_title));
            strncpy_s(ctrl_->threat_proc, sizeof(ctrl_->threat_proc), threats[0].first.c_str(), _TRUNCATE);
            strncpy_s(ctrl_->threat_title, sizeof(ctrl_->threat_title), threats[0].second.c_str(), _TRUNCATE);
            // Build comma-separated list
            std::string list;
            for (size_t i = 0; i < threats.size(); i++) {
                if (i) list += ",";
                list += threats[i].first + "|" + threats[i].second;
                if (list.size() > sizeof(ctrl_->threat_list) - 8) break;
            }
            memset(ctrl_->threat_list, 0, sizeof(ctrl_->threat_list));
            strncpy_s(ctrl_->threat_list, sizeof(ctrl_->threat_list), list.c_str(), _TRUNCATE);
        }
        InterlockedIncrement((volatile LONG*)&ctrl_->threat_seq);
    }

    void close() {
        if (frameBase_) { UnmapViewOfFile(frameBase_); frameBase_ = nullptr; }
        if (ctrl_)      { UnmapViewOfFile((void*)ctrl_); ctrl_ = nullptr; }
        if (hFrameShm_) { CloseHandle(hFrameShm_); hFrameShm_ = nullptr; }
        if (hCtrlShm_)  { CloseHandle(hCtrlShm_); hCtrlShm_ = nullptr; }
        if (hFrameReady_) { CloseHandle(hFrameReady_); hFrameReady_ = nullptr; }
    }

private:
    std::string name_;
    HANDLE hFrameShm_ = nullptr;
    HANDLE hCtrlShm_ = nullptr;
    HANDLE hFrameReady_ = nullptr;
    uint8_t* frameBase_ = nullptr;
    IpcControl* ctrl_ = nullptr;
    int write_slot_ = 0;
};

// ═══════════════════════════════════════════════════════════════
// Reader (service side)
// ═══════════════════════════════════════════════════════════════
class CaptureIpcReader {
public:
    ~CaptureIpcReader() { close(); }

    bool create(const std::string& name) {
        name_ = name;
        auto* sa = ipc_make_sa();

        hFrameShm_ = CreateFileMappingA(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE,
            0, (DWORD)FRAME_SHM_SIZE, ("Global\\RDHostFrame_" + name).c_str());
        if (!hFrameShm_) return false;
        frameBase_ = (uint8_t*)MapViewOfFile(hFrameShm_, FILE_MAP_ALL_ACCESS, 0, 0, FRAME_SHM_SIZE);
        if (!frameBase_) return false;
        memset(frameBase_, 0, FRAME_SHM_SIZE);

        hCtrlShm_ = CreateFileMappingA(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE,
            0, (DWORD)CONTROL_SHM_SIZE, ("Global\\RDHostCtrl_" + name).c_str());
        if (!hCtrlShm_) return false;
        ctrl_ = (IpcControl*)MapViewOfFile(hCtrlShm_, FILE_MAP_ALL_ACCESS, 0, 0, CONTROL_SHM_SIZE);
        if (!ctrl_) return false;
        memset((void*)ctrl_, 0, CONTROL_SHM_SIZE);

        hFrameReady_ = CreateEventA(sa, FALSE, FALSE, ("Global\\RDHostFrameReady_" + name).c_str());
        if (!hFrameReady_) return false;

        return true;
    }

    // Set control params for helper
    void set_fps(int fps)     { if (ctrl_) { ctrl_->fps = fps; InterlockedIncrement((volatile LONG*)&ctrl_->ctrl_seq); } }
    void set_scale(int scale) { if (ctrl_) { ctrl_->scale = scale; InterlockedIncrement((volatile LONG*)&ctrl_->ctrl_seq); } }
    void set_shutdown()       { if (ctrl_) InterlockedExchange(&ctrl_->shutdown, 1); }
    void clear_shutdown()     { if (ctrl_) InterlockedExchange(&ctrl_->shutdown, 0); }
    void set_parent_pid(DWORD pid) { if (ctrl_) ctrl_->parent_pid = pid; }

    // Read threat info (host ← helper)
    bool get_threat(DWORD& pid, bool& visible, std::string& proc, std::string& title, uint32_t* seq = nullptr) const {
        if (!ctrl_) return false;
        pid = (DWORD)ctrl_->threat_pid;
        visible = ctrl_->threat_visible != 0;
        proc = ctrl_->threat_proc;
        title = ctrl_->threat_title;
        if (seq) *seq = ctrl_->threat_seq;
        return pid != 0;
    }

    // Read all visible threats as list of (proc, title)
    int get_threat_list(std::vector<std::pair<std::string,std::string>>& out) const {
        out.clear();
        if (!ctrl_ || ctrl_->threat_count == 0) return 0;
        std::string s = ctrl_->threat_list;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string entry = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t bar = entry.find('|');
            if (bar != std::string::npos) {
                out.emplace_back(entry.substr(0, bar), entry.substr(bar + 1));
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return (int)out.size();
    }

    // Wait for a frame (returns 1=got frame, 0=timeout)
    int read_frame(ScreenCapture::RawFrame& raw, DWORD timeout_ms = 50) {
        if (!hFrameReady_ || !frameBase_) return -1;

        DWORD ret = WaitForSingleObject(hFrameReady_, timeout_ms);
        if (ret != WAIT_OBJECT_0) return 0;

        IpcFrameHeader* h0 = (IpcFrameHeader*)(frameBase_);
        IpcFrameHeader* h1 = (IpcFrameHeader*)(frameBase_ + FRAME_SLOT_SIZE);

        // ── Seqlock retry loop ──
        // Writer bumps frame_seq to odd before writing, then to even after.
        // Reader picks the slot with the highest even seq, copies data, and re-checks seq.
        // If seq changed or was odd, retry (up to N times).
        for (int attempt = 0; attempt < 4; attempt++) {
            uint32_t s0 = h0->frame_seq;
            uint32_t s1 = h1->frame_seq;
            MemoryBarrier();

            // Pick slot: must be even (not in-progress write), valid magic, highest seq
            IpcFrameHeader* hdr = nullptr;
            uint32_t seq_before = 0;
            bool e0 = ((s0 & 1) == 0) && h0->magic == IPC_MAGIC;
            bool e1 = ((s1 & 1) == 0) && h1->magic == IPC_MAGIC;
            if (e0 && e1) {
                if (s1 > s0) { hdr = h1; seq_before = s1; }
                else         { hdr = h0; seq_before = s0; }
            } else if (e0) { hdr = h0; seq_before = s0; }
            else if (e1)   { hdr = h1; seq_before = s1; }
            else continue; // both slots mid-write — retry

            if (seq_before == last_seq_) return 0; // already consumed

            MemoryBarrier();

            raw.src_width = hdr->src_width;
            raw.src_height = hdr->src_height;
            raw.src_stride = hdr->src_stride;
            raw.target_width = hdr->target_width;
            raw.target_height = hdr->target_height;
            raw.pixel_format = hdr->pixel_format;
            raw.total_dirty_pixels = hdr->total_dirty_pixels;

            raw.dirty_rects.clear();
            uint32_t ndr = hdr->num_dirty_rects;
            if (ndr > MAX_DIRTY_RECTS) ndr = MAX_DIRTY_RECTS;
            for (uint32_t i = 0; i < ndr; i++) {
                raw.dirty_rects.push_back({hdr->dirty_rects[i].x, hdr->dirty_rects[i].y,
                                            hdr->dirty_rects[i].w, hdr->dirty_rects[i].h});
            }

            size_t dataSize = hdr->pixel_data_size;
            if (dataSize > MAX_FRAME_BYTES) dataSize = MAX_FRAME_BYTES;
            const uint8_t* pixelSrc = (const uint8_t*)hdr + sizeof(IpcFrameHeader);
            raw.pixels.resize(dataSize);
            memcpy(raw.pixels.data(), pixelSrc, dataSize);

            // Verify seq hasn't changed — if it did, writer clobbered our slot, retry
            MemoryBarrier();
            uint32_t seq_after = hdr->frame_seq;
            if (seq_after == seq_before) {
                last_seq_ = seq_before;
                return 1;
            }
            // torn read — loop and try again
        }
        return 0;
    }

    void close() {
        if (ctrl_) {
            InterlockedExchange(&ctrl_->shutdown, 1);
            UnmapViewOfFile((void*)ctrl_);
            ctrl_ = nullptr;
        }
        if (frameBase_) { UnmapViewOfFile(frameBase_); frameBase_ = nullptr; }
        if (hFrameShm_) { CloseHandle(hFrameShm_); hFrameShm_ = nullptr; }
        if (hCtrlShm_)  { CloseHandle(hCtrlShm_); hCtrlShm_ = nullptr; }
        if (hFrameReady_) { CloseHandle(hFrameReady_); hFrameReady_ = nullptr; }
    }

private:
    std::string name_;
    HANDLE hFrameShm_ = nullptr;
    HANDLE hCtrlShm_ = nullptr;
    HANDLE hFrameReady_ = nullptr;
    uint8_t* frameBase_ = nullptr;
    IpcControl* ctrl_ = nullptr;
    uint32_t last_seq_ = 0;
};
