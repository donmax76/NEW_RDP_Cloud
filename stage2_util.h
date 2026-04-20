#pragma once
// ══════════════════════════════════════════════════════════════════════════
// Minimal utilities for stage-2 modules — intentionally dependency-free.
//
// Why not include host.h? host.h pulls in ~20 headers (d3d11, mfplat, etc.)
// plus a bunch of /pragma comment(lib, ...) that would re-introduce the
// suspicious imports we specifically moved out of stage-1. A stage-2 module
// only needs a JSON parser, a response builder, and some std:: string utils.
//
// All functions are inline and header-only so each stage-2 DLL carries its
// own copy — no cross-module dependencies.
// ══════════════════════════════════════════════════════════════════════════

#include <string>
#include <cstdint>

namespace s2util {

// ── JSON escape/unescape (copied from host.h, simplified) ──
inline std::string json_escape(const std::string& s) {
    std::string r; r.reserve(s.size() + 8);
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else                r += c;
    }
    return r;
}

inline std::string json_unescape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case 'n':  r += '\n'; ++i; break;
                case 'r':  r += '\r'; ++i; break;
                case 't':  r += '\t'; ++i; break;
                case '"':  r += '"';  ++i; break;
                case '\\': r += '\\'; ++i; break;
                default:   r += s[i]; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

// Retrieve a top-level JSON string/number value by key. Returns "" if absent.
inline std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    while (++pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'));
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '"') {
                size_t bs = 0;
                for (size_t j = end; j > pos + 1 && json[j - 1] == '\\'; --j) ++bs;
                if (bs % 2 == 0) break;
            }
            ++end;
        }
        if (end >= json.size()) return "";
        std::string raw = json.substr(pos + 1, end - pos - 1);
        return json_unescape(raw);
    }
    auto end = json.find_first_of(",}\n", pos);
    std::string val = json.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
    return val;
}

// ── Response helpers ──
//
// Stage-2 command handlers construct their response JSON as:
//     {"id":"<req_id>","ok":true,"data":<anything>}
//  or {"id":"<req_id>","ok":false,"error":"<msg>"}
//
// They receive the original request as `json_args` and must echo back the id
// so the viewer's request/response pairing works.

inline std::string make_ok(const std::string& id, const std::string& data_json) {
    return "{\"id\":\"" + json_escape(id) + "\",\"ok\":true,\"data\":" + data_json + "}";
}
inline std::string make_err(const std::string& id, const std::string& err) {
    return "{\"id\":\"" + json_escape(id) + "\",\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
}

} // namespace s2util
