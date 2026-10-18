#include "PCH.h"
#include "Network/Crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

// End-to-end crypto via Windows CNG. ECDH P-256 for key agreement, AES-256-GCM for
// authenticated packet encryption. No third-party dependency. See Crypto.h.
namespace Pulsar::Network
{
    namespace
    {
        inline bool Ok(NTSTATUS s) { return s >= 0; }

        // RAII helpers for the algorithm providers (opened on demand, closed at scope end).
        struct AlgProvider
        {
            BCRYPT_ALG_HANDLE h = nullptr;
            explicit AlgProvider(LPCWSTR id) { BCryptOpenAlgorithmProvider(&h, id, nullptr, 0); }
            ~AlgProvider() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
        };
    }

    CryptoChannel::~CryptoChannel() { Reset(); }

    void CryptoChannel::Reset()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_hAesKey)    { BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hAesKey)); m_hAesKey = nullptr; }
        if (m_hMyKeyPair) { BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hMyKeyPair)); m_hMyKeyPair = nullptr; }
        m_pubBlob.clear();
        m_ready = false;
    }

    bool CryptoChannel::Init()
    {
        Reset();
        std::lock_guard<std::mutex> lk(m_mtx);

        AlgProvider ecdh(BCRYPT_ECDH_P256_ALGORITHM);
        if (!ecdh.h) return false;

        BCRYPT_KEY_HANDLE key = nullptr;
        if (!Ok(BCryptGenerateKeyPair(ecdh.h, &key, 256, 0))) return false;
        if (!Ok(BCryptFinalizeKeyPair(key, 0))) { BCryptDestroyKey(key); return false; }

        // Export our public blob (BCRYPT_ECCKEY_BLOB header + X + Y).
        ULONG cb = 0;
        if (!Ok(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cb, 0)) || cb == 0) {
            BCryptDestroyKey(key); return false;
        }
        std::vector<uint8_t> blob(cb);
        if (!Ok(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), cb, &cb, 0))) {
            BCryptDestroyKey(key); return false;
        }
        blob.resize(cb);

        m_hMyKeyPair = key;
        m_pubBlob    = std::move(blob);
        return true;
    }

    std::vector<uint8_t> CryptoChannel::PublicBlob() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_pubBlob;
    }

    void CryptoChannel::SetAuthSecret(const uint8_t* a_secret, size_t a_len)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (a_secret && a_len) m_authSecret.assign(a_secret, a_secret + a_len);
        else                   m_authSecret.clear();
    }

    bool CryptoChannel::Ready() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_ready;
    }

    bool CryptoChannel::DeriveFromPeer(const uint8_t* a_peerBlob, size_t a_len)
    {
        if (!a_peerBlob || a_len == 0) return false;
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_hMyKeyPair) return false;

        AlgProvider ecdh(BCRYPT_ECDH_P256_ALGORITHM);
        if (!ecdh.h) return false;

        BCRYPT_KEY_HANDLE peer = nullptr;
        if (!Ok(BCryptImportKeyPair(ecdh.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer,
                                    const_cast<PUCHAR>(a_peerBlob), static_cast<ULONG>(a_len), 0)))
            return false;

        BCRYPT_SECRET_HANDLE secret = nullptr;
        if (!Ok(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(m_hMyKeyPair), peer, &secret, 0))) {
            BCryptDestroyKey(peer); return false;
        }

        // KDF: 32-byte AES key = SHA-256( [authSecret] || ECDH-shared-secret ). Deterministic,
        // so both peers derive the identical key. When an auth secret is set (delivered over
        // the TRUSTED DB/TLS channel, never via the relay) it is PREPENDED into the hash:
        // a MITM relay can substitute the ECDH public keys but does not know the secret, so
        // it cannot reproduce the key (active-MITM defence). No secret = legacy hash of the
        // ECDH secret alone (passive-only; used where there is no DB channel + no relay).
        wchar_t hashName[] = L"SHA256";
        BCryptBuffer kp[2]{};
        kp[0].BufferType = KDF_HASH_ALGORITHM;
        kp[0].cbBuffer   = sizeof(hashName);
        kp[0].pvBuffer   = hashName;
        ULONG nbuf = 1;
        if (!m_authSecret.empty())
        {
            kp[1].BufferType = KDF_SECRET_PREPEND;
            kp[1].cbBuffer   = static_cast<ULONG>(m_authSecret.size());
            kp[1].pvBuffer   = m_authSecret.data();
            nbuf = 2;
        }
        BCryptBufferDesc params{};
        params.ulVersion = BCRYPTBUFFER_VERSION;
        params.cBuffers  = nbuf;
        params.pBuffers  = kp;

        uint8_t keyBytes[32] = {};
        ULONG cbKey = 0;
        NTSTATUS st = BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &params, keyBytes, sizeof(keyBytes), &cbKey, 0);
        BCryptDestroySecret(secret);
        BCryptDestroyKey(peer);
        if (!Ok(st) || cbKey != sizeof(keyBytes)) { SecureZeroMemory(keyBytes, sizeof(keyBytes)); return false; }

        // Create the AES-256-GCM key handle.
        AlgProvider aes(BCRYPT_AES_ALGORITHM);
        if (!aes.h) { SecureZeroMemory(keyBytes, sizeof(keyBytes)); return false; }
        if (!Ok(BCryptSetProperty(aes.h, BCRYPT_CHAINING_MODE,
                                  (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            SecureZeroMemory(keyBytes, sizeof(keyBytes)); return false;
        }
        BCRYPT_KEY_HANDLE aesKey = nullptr;
        st = BCryptGenerateSymmetricKey(aes.h, &aesKey, nullptr, 0, keyBytes, sizeof(keyBytes), 0);
        SecureZeroMemory(keyBytes, sizeof(keyBytes));
        if (!Ok(st)) return false;

        if (m_hAesKey) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hAesKey));
        m_hAesKey = aesKey;
        m_ready   = true;
        return true;
    }

    bool CryptoChannel::Encrypt(const uint8_t* a_pt, size_t a_len, std::vector<uint8_t>& a_out)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_ready || !m_hAesKey) return false;

        a_out.resize(kNonceLen + a_len + kTagLen);
        uint8_t* nonce = a_out.data();
        uint8_t* ct    = a_out.data() + kNonceLen;
        uint8_t* tag   = a_out.data() + kNonceLen + a_len;

        if (!Ok(BCryptGenRandom(nullptr, nonce, (ULONG)kNonceLen, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            { a_out.clear(); return false; }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = (ULONG)kNonceLen;
        info.pbTag   = tag;
        info.cbTag   = (ULONG)kTagLen;

        ULONG written = 0;
        NTSTATUS st = BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(m_hAesKey),
                                    const_cast<PUCHAR>(a_pt), (ULONG)a_len, &info,
                                    nullptr, 0, ct, (ULONG)a_len, &written, 0);
        if (!Ok(st)) { a_out.clear(); return false; }
        return true;
    }

    bool CryptoChannel::Decrypt(const uint8_t* a_in, size_t a_len, std::vector<uint8_t>& a_out)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        a_out.clear();
        if (!m_ready || !m_hAesKey) return false;
        if (a_len < kNonceLen + kTagLen) return false;

        const size_t ctLen = a_len - kNonceLen - kTagLen;
        const uint8_t* nonce = a_in;
        const uint8_t* ct    = a_in + kNonceLen;
        const uint8_t* tag   = a_in + kNonceLen + ctLen;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = (ULONG)kNonceLen;
        info.pbTag   = const_cast<PUCHAR>(tag);
        info.cbTag   = (ULONG)kTagLen;

        a_out.resize(ctLen);
        ULONG written = 0;
        // Never pass a null output buffer: CNG treats that as a size query and returns
        // success WITHOUT verifying the tag, so any 28-byte (empty-payload) packet
        // would be accepted as authentic. Point at a dummy byte when ctLen == 0.
        UCHAR dummy = 0;
        NTSTATUS st = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_hAesKey),
                                    const_cast<PUCHAR>(ct), (ULONG)ctLen, &info,
                                    nullptr, 0, ctLen ? a_out.data() : &dummy, (ULONG)ctLen, &written, 0);
        if (!Ok(st)) { a_out.clear(); return false; }   // STATUS_AUTH_TAG_MISMATCH on tamper
        a_out.resize(written);
        return true;
    }

    bool CryptoChannel::SelfTest()
    {
        CryptoChannel a, b;
        if (!a.Init() || !b.Init()) return false;
        auto pa = a.PublicBlob();
        auto pb = b.PublicBlob();
        if (pa.empty() || pb.empty()) return false;
        if (!a.DeriveFromPeer(pb.data(), pb.size())) return false;
        if (!b.DeriveFromPeer(pa.data(), pa.size())) return false;

        const char* msg = "PULSAR e2e self-test payload \x01\x02\x03";
        const auto* pt = reinterpret_cast<const uint8_t*>(msg);
        const size_t n = std::strlen(msg);

        std::vector<uint8_t> enc, dec;
        if (!a.Encrypt(pt, n, enc)) return false;
        if (!b.Decrypt(enc.data(), enc.size(), dec)) return false;
        if (dec.size() != n || std::memcmp(dec.data(), pt, n) != 0) return false;

        // Reverse direction too.
        std::vector<uint8_t> enc2, dec2;
        if (!b.Encrypt(pt, n, enc2)) return false;
        if (!a.Decrypt(enc2.data(), enc2.size(), dec2)) return false;
        if (dec2.size() != n || std::memcmp(dec2.data(), pt, n) != 0) return false;

        // Tamper must be rejected.
        if (enc.size() > kNonceLen) {
            enc[kNonceLen] ^= 0xFF;   // flip a ciphertext byte
            std::vector<uint8_t> bad;
            if (b.Decrypt(enc.data(), enc.size(), bad)) return false;  // MUST fail
        }

        // Authenticated (anti-MITM) agreement:
        //  (1) SAME shared secret on both sides -> same key -> round-trips.
        //  (2) DIFFERENT secret on the receiver (= a MITM that lacks the real DB secret)
        //      -> different key -> decryption MUST fail. This is the property that stops a
        //      malicious relay operator from reading "E2E" traffic.
        {
            const uint8_t secret[] = { 'p','u','l','s','a','r', 1,2,3,4,5,6,7,8,9,10 };
            CryptoChannel s1, s2;
            if (!s1.Init() || !s2.Init()) return false;
            s1.SetAuthSecret(secret, sizeof(secret));
            s2.SetAuthSecret(secret, sizeof(secret));
            auto sp1 = s1.PublicBlob(); auto sp2 = s2.PublicBlob();
            if (!s1.DeriveFromPeer(sp2.data(), sp2.size())) return false;
            if (!s2.DeriveFromPeer(sp1.data(), sp1.size())) return false;
            std::vector<uint8_t> se, sd;
            if (!s1.Encrypt(pt, n, se)) return false;
            if (!s2.Decrypt(se.data(), se.size(), sd)) return false;   // matching secret -> OK
            if (sd.size() != n || std::memcmp(sd.data(), pt, n) != 0) return false;

            const uint8_t wrong[] = { 'a','t','t','a','c','k','e','r', 0,0,0,0,0,0,0,0 };
            CryptoChannel w1, w2;
            if (!w1.Init() || !w2.Init()) return false;
            w1.SetAuthSecret(secret, sizeof(secret));
            w2.SetAuthSecret(wrong,  sizeof(wrong));   // MITM does not have the real secret
            auto wp1 = w1.PublicBlob(); auto wp2 = w2.PublicBlob();
            if (!w1.DeriveFromPeer(wp2.data(), wp2.size())) return false;
            if (!w2.DeriveFromPeer(wp1.data(), wp1.size())) return false;
            std::vector<uint8_t> we, wbad;
            if (!w1.Encrypt(pt, n, we)) return false;
            if (w2.Decrypt(we.data(), we.size(), wbad)) return false;  // mismatched secret -> MUST fail
        }
        return true;
    }
}
