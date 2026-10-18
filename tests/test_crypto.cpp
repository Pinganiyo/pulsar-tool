// End-to-end encryption channel (src/Network/Crypto.cpp): ECDH P-256 key
// agreement + AES-256-GCM, via Windows CNG.

#include "TestHarness.h"

#include "Network/Crypto.h"

#include <string>
#include <vector>

using Pulsar::Network::CryptoChannel;

namespace {

    bool Pair(CryptoChannel& a, CryptoChannel& b)
    {
        if (!a.Init() || !b.Init()) return false;
        auto pa = a.PublicBlob();
        auto pb = b.PublicBlob();
        return a.DeriveFromPeer(pb.data(), pb.size()) && b.DeriveFromPeer(pa.data(), pa.size());
    }

    std::vector<uint8_t> Msg(const std::string& s) { return { s.begin(), s.end() }; }
}

TEST(Crypto, BuiltInSelfTestPasses)
{
    CHECK(CryptoChannel::SelfTest());
}

TEST(Crypto, NotReadyUntilKeyAgreement)
{
    CryptoChannel a;
    CHECK(!a.Ready());
    REQUIRE(a.Init());
    CHECK(!a.Ready());
    std::vector<uint8_t> out;
    auto m = Msg("x");
    CHECK(!a.Encrypt(m.data(), m.size(), out));
    CHECK(!a.Decrypt(m.data(), m.size(), out));
}

TEST(Crypto, RoundTripBothDirections)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    CHECK(a.Ready());
    CHECK(b.Ready());
    auto m = Msg("position packet payload");
    std::vector<uint8_t> enc, dec;
    REQUIRE(a.Encrypt(m.data(), m.size(), enc));
    CHECK_EQ(enc.size(), m.size() + CryptoChannel::kOverhead);
    REQUIRE(b.Decrypt(enc.data(), enc.size(), dec));
    CHECK(dec == m);
    REQUIRE(b.Encrypt(m.data(), m.size(), enc));
    REQUIRE(a.Decrypt(enc.data(), enc.size(), dec));
    CHECK(dec == m);
}

TEST(Crypto, EmptyAndLargePayloads)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    std::vector<uint8_t> enc, dec;
    REQUIRE(a.Encrypt(nullptr, 0, enc));
    REQUIRE(b.Decrypt(enc.data(), enc.size(), dec));
    CHECK(dec.empty());

    std::vector<uint8_t> big(256 * 1024);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31);
    REQUIRE(a.Encrypt(big.data(), big.size(), enc));
    REQUIRE(b.Decrypt(enc.data(), enc.size(), dec));
    CHECK(dec == big);
}

TEST(Crypto, SamePlaintextGivesDifferentCiphertext)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    auto m = Msg("same message");
    std::vector<uint8_t> e1, e2;
    REQUIRE(a.Encrypt(m.data(), m.size(), e1));
    REQUIRE(a.Encrypt(m.data(), m.size(), e2));
    CHECK(e1 != e2);   // fresh nonce per packet
}

TEST(Crypto, AnyFlippedByteIsRejected)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    auto m = Msg("tamper me");
    std::vector<uint8_t> enc;
    REQUIRE(a.Encrypt(m.data(), m.size(), enc));
    int accepted = 0;
    for (size_t i = 0; i < enc.size(); ++i) {   // nonce, ciphertext and tag
        auto bad = enc;
        bad[i] ^= 0x01;
        std::vector<uint8_t> dec;
        if (b.Decrypt(bad.data(), bad.size(), dec)) ++accepted;
        CHECK(dec.empty());
    }
    CHECK_EQ(accepted, 0);
}

TEST(Crypto, ShortOrTruncatedInputIsRejected)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    auto m = Msg("truncate me");
    std::vector<uint8_t> enc, dec;
    REQUIRE(a.Encrypt(m.data(), m.size(), enc));
    for (size_t len = 0; len < enc.size(); ++len) {
        if (b.Decrypt(enc.data(), len, dec))
            ::pt::Fail(__FILE__, __LINE__, "accepted truncated length " + std::to_string(len) +
                                               " of " + std::to_string(enc.size()));
    }
}

TEST(Crypto, ForgedEmptyPacketIsRejected)
{
    // Regression: a 28-byte packet (nonce + tag, no body) used to be accepted
    // without tag verification because Decrypt handed CNG a null output buffer.
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    std::vector<uint8_t> forged(CryptoChannel::kOverhead, 0x42), dec;
    CHECK(!b.Decrypt(forged.data(), forged.size(), dec));

    // A genuine empty packet still decrypts.
    std::vector<uint8_t> enc;
    REQUIRE(a.Encrypt(nullptr, 0, enc));
    CHECK(b.Decrypt(enc.data(), enc.size(), dec));
    enc.back() ^= 1;
    CHECK(!b.Decrypt(enc.data(), enc.size(), dec));
}

TEST(Crypto, ThirdPartyCannotDecrypt)
{
    CryptoChannel a, b, eve, eve2;
    REQUIRE(Pair(a, b));
    REQUIRE(Pair(eve, eve2));
    auto m = Msg("secret");
    std::vector<uint8_t> enc, dec;
    REQUIRE(a.Encrypt(m.data(), m.size(), enc));
    CHECK(!eve.Decrypt(enc.data(), enc.size(), dec));
}

TEST(Crypto, AuthSecretMustMatch)
{
    const uint8_t secret[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t other[]  = { 8, 7, 6, 5, 4, 3, 2, 1 };
    auto m = Msg("authenticated");

    // Same order as NetworkHost: Init() first, then the secret, then derive.
    CryptoChannel a, b;
    REQUIRE(a.Init());
    REQUIRE(b.Init());
    a.SetAuthSecret(secret, sizeof(secret));
    b.SetAuthSecret(secret, sizeof(secret));
    auto pa = a.PublicBlob(), pb = b.PublicBlob();
    REQUIRE(a.DeriveFromPeer(pb.data(), pb.size()));
    REQUIRE(b.DeriveFromPeer(pa.data(), pa.size()));
    std::vector<uint8_t> enc, dec;
    REQUIRE(a.Encrypt(m.data(), m.size(), enc));
    CHECK(b.Decrypt(enc.data(), enc.size(), dec));

    CryptoChannel c, d;
    REQUIRE(c.Init());
    REQUIRE(d.Init());
    c.SetAuthSecret(secret, sizeof(secret));
    d.SetAuthSecret(other, sizeof(other));
    auto pc = c.PublicBlob(), pd = d.PublicBlob();
    REQUIRE(c.DeriveFromPeer(pd.data(), pd.size()));
    REQUIRE(d.DeriveFromPeer(pc.data(), pc.size()));
    REQUIRE(c.Encrypt(m.data(), m.size(), enc));
    CHECK(!d.Decrypt(enc.data(), enc.size(), dec));
}

TEST(Crypto, GarbagePeerKeyIsRejected)
{
    CryptoChannel a;
    REQUIRE(a.Init());
    std::vector<uint8_t> junk(72, 0xAB);
    CHECK(!a.DeriveFromPeer(junk.data(), junk.size()));
    CHECK(!a.Ready());
    CHECK(!a.DeriveFromPeer(nullptr, 0));
}

TEST(Crypto, ResetDropsTheSession)
{
    CryptoChannel a, b;
    REQUIRE(Pair(a, b));
    a.Reset();
    CHECK(!a.Ready());
    auto m = Msg("after reset");
    std::vector<uint8_t> enc;
    CHECK(!a.Encrypt(m.data(), m.size(), enc));
}
