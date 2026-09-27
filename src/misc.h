// MaiEngine v3 — küçük yardımcılar
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <chrono>
#include <cstdint>

namespace mai {

// xorshift64* — hızlı, deterministik rastgele sayı üreteci
class PRNG {
    uint64_t s;

public:
    explicit PRNG(uint64_t seed) : s(seed) {}
    uint64_t rand64() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    // Az bitli sayı: magic aramasında daha hızlı sonuç verir
    uint64_t sparse_rand() { return rand64() & rand64() & rand64(); }
};

using TimePoint = int64_t;  // milisaniye
inline TimePoint now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace mai
