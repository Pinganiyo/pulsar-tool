// Hello-vs-encrypted classification (include/Network/KeyExchange.h).
// Regression: ~1 in 256 encrypted packets start with 0xA1 and were dropped.

#include "TestHarness.h"

#include "Network/Crypto.h"
#include "Network/KeyExchange.h"

#include <vector>

using namespace Pulsar::Network;

TEST(KeyExchange, HelloIsRecognisedBeforeTheKeyIsAgreed)
{
    std::vector<uint8_t> hello = { PKT_CRYPTO_HELLO, 1, 2, 3 };
    CHECK(IsKeyExchangeHello(false, hello.data(), hello.size()));
    CHECK(!IsKeyExchangeHello(false, hello.data(), 1));          // no key bytes
    CHECK(!IsKeyExchangeHello(false, nullptr, 0));
}

TEST(KeyExchange, EncryptedPacketsStartingWith0xA1AreNotHellos)
{
    CryptoChannel a, b;
    REQUIRE(a.Init() && b.Init());
    auto pa = a.PublicBlob(), pb = b.PublicBlob();
    REQUIRE(a.DeriveFromPeer(pb.data(), pb.size()) && b.DeriveFromPeer(pa.data(), pa.size()));

    // Encrypt until the random nonce starts with the hello byte (~1 in 256).
    const std::vector<uint8_t> msg = { PKT_POSITION, 0, 0, 7, 7, 7 };
    std::vector<uint8_t> enc;
    int tries = 0;
    do {
        REQUIRE(a.Encrypt(msg.data(), msg.size(), enc));
    } while (enc[0] != PKT_CRYPTO_HELLO && ++tries < 20000);
    REQUIRE(enc[0] == PKT_CRYPTO_HELLO);

    // Once the session is up it is traffic, and it decrypts fine.
    CHECK(!IsKeyExchangeHello(b.Ready(), enc.data(), enc.size()));
    std::vector<uint8_t> dec;
    CHECK(b.Decrypt(enc.data(), enc.size(), dec));
    CHECK(dec == msg);
}
