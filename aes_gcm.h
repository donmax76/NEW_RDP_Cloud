#pragma once
// ══════════════════════════════════════════════════════════════════════════
// AES-256-GCM encrypt/decrypt for stage-2 blobs.
// Built on OpenSSL EVP (already linked into stage-1 for TLS).
//
// Blob format on disk:
//     [12 bytes IV] [N bytes ciphertext] [16 bytes GCM tag]
//
// Key is always 32 bytes (AES-256). Derived from room_token via SHA-256
// with a fixed context string (domain separation so the same room_token
// isn't reused as an encryption key elsewhere).
// ══════════════════════════════════════════════════════════════════════════

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

namespace aesgcm {

static constexpr size_t KEY_LEN = 32;  // AES-256
static constexpr size_t IV_LEN  = 12;  // GCM standard
static constexpr size_t TAG_LEN = 16;  // GCM standard

// ── Derive 32-byte key from room_token (SHA-256 with domain separator) ──
inline std::vector<uint8_t> derive_key(const std::string& room_token) {
    static const char kCtx[] = "pnp.stage2.v1";
    uint8_t out[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, kCtx, sizeof(kCtx) - 1);
    SHA256_Update(&ctx, room_token.data(), room_token.size());
    SHA256_Final(out, &ctx);
    return std::vector<uint8_t>(out, out + SHA256_DIGEST_LENGTH);
}

// ── Encrypt: returns [IV || ciphertext || tag] ──
// Used only by the offline _gen_stage2_blob.py equivalent in C++ (for tests).
inline bool encrypt(const uint8_t* key, size_t key_len,
                    const uint8_t* pt, size_t pt_len,
                    std::vector<uint8_t>& blob_out) {
    if (key_len != KEY_LEN) return false;

    blob_out.resize(IV_LEN + pt_len + TAG_LEN);
    uint8_t* iv  = blob_out.data();
    uint8_t* ct  = blob_out.data() + IV_LEN;
    uint8_t* tag = blob_out.data() + IV_LEN + pt_len;

    if (RAND_bytes(iv, IV_LEN) != 1) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outlen = 0, tmplen = 0;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) break;
        if (EVP_EncryptUpdate(ctx, ct, &outlen, pt, (int)pt_len) != 1) break;
        if (EVP_EncryptFinal_ex(ctx, ct + outlen, &tmplen) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1) break;
        ok = true;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// ── Decrypt: accepts [IV || ciphertext || tag], returns plaintext ──
// Returns false on any error (wrong key, truncated blob, tampered tag).
inline bool decrypt(const uint8_t* key, size_t key_len,
                    const uint8_t* blob, size_t blob_len,
                    std::vector<uint8_t>& pt_out) {
    if (key_len != KEY_LEN) return false;
    if (blob_len < IV_LEN + TAG_LEN) return false;

    size_t ct_len = blob_len - IV_LEN - TAG_LEN;
    const uint8_t* iv  = blob;
    const uint8_t* ct  = blob + IV_LEN;
    const uint8_t* tag = blob + IV_LEN + ct_len;

    pt_out.resize(ct_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outlen = 0, tmplen = 0;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) break;
        if (EVP_DecryptUpdate(ctx, pt_out.data(), &outlen, ct, (int)ct_len) != 1) break;
        // Set expected tag BEFORE calling Final
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, (void*)tag) != 1) break;
        if (EVP_DecryptFinal_ex(ctx, pt_out.data() + outlen, &tmplen) != 1) break;  // returns 0 on tag mismatch
        ok = true;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        // Wipe partial plaintext on failure to avoid leaking data on tag mismatch.
        std::memset(pt_out.data(), 0, pt_out.size());
        pt_out.clear();
    }
    return ok;
}

} // namespace aesgcm
