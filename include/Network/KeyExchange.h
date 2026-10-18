#pragma once

// Is this incoming datagram the plaintext key-exchange hello, or encrypted
// traffic? Game-free (tests/test_key_exchange.cpp).
//
// The hello is [PKT_CRYPTO_HELLO][public key]; every other packet is
// [12-byte random nonce][ciphertext][tag]. Checking only data[0] misreads about
// 1 in 256 encrypted packets (nonce byte 0 == 0xA1) as a hello - before 0.7.3
// NetworkHost did exactly that and silently dropped those packets, including
// reliable ones ENet had already acknowledged (loot, quest stages, the save
// transfer...). A hello is only valid while the session key is not agreed yet;
// a reconnect resets the channel before the next one arrives.

#include "Network/PacketTypes.h"

#include <cstddef>
#include <cstdint>

namespace Pulsar::Network {

    inline bool IsKeyExchangeHello(bool a_sessionReady, const std::uint8_t* a_data, std::size_t a_len)
    {
        return !a_sessionReady && a_data && a_len > 1 && a_data[0] == PKT_CRYPTO_HELLO;
    }
}
