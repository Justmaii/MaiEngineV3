// MaiEngine v3 — transposition table (kovalı, yaşlı)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Her kova 32 bayt: 3 kayıt x 10 bayt + 2 bayt dolgu. Bir kova tek önbellek
// satırının yarısına sığar. Kayıt: anahtarın 16 biti, derinlik, yaş+PV+sınır,
// hamle, değer, statik değerlendirme.
// Değiştirme: aynı anahtar yoksa "derinlik - 8 * yaş" en küçük olan gider.
// Lazy SMP'de kilitsiz; yarış olursa en kötü ihtimalle hamle legal_move ile elenir.
#pragma once

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace mai {

enum Bound : uint8_t { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

struct TTEntry {
    Move move() const { return Move(move16); }
    Value value() const { return value16; }
    Value eval() const { return eval16; }
    Depth depth() const { return Depth(depth8) + DEPTH_OFFSET; }
    bool is_pv() const { return genBound8 & 0x4; }
    Bound bound() const { return Bound(genBound8 & 0x3); }
    void save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev);

private:
    friend class TranspositionTable;
    uint16_t key16;
    uint8_t depth8;
    uint8_t genBound8;
    uint16_t move16;
    int16_t value16;
    int16_t eval16;
};

class TranspositionTable {
    static constexpr int ClusterSize = 3;
    struct Cluster {
        TTEntry entry[ClusterSize];
        char padding[2];
    };
    static_assert(sizeof(Cluster) == 32, "Kova 32 bayt olmalı");

    // Yaş: genBound8'in üst 5 biti. Sınır 2 bit + PV 1 bit = 3 bit
    static constexpr unsigned GENERATION_BITS = 3;
    static constexpr int GENERATION_DELTA = 1 << GENERATION_BITS;
    static constexpr int GENERATION_CYCLE = 255 + GENERATION_DELTA;
    static constexpr int GENERATION_MASK = (0xFF << GENERATION_BITS) & 0xFF;

public:
    ~TranspositionTable();
    void resize(size_t mb);
    void clear();
    void new_search() { generation8 += GENERATION_DELTA; }
    uint8_t generation() const { return generation8; }
    TTEntry* probe(Key key, bool& found) const;
    int hashfull() const;  // binde doluluk
    void prefetch(Key key) const { __builtin_prefetch(first_entry(key)); }

private:
    friend struct TTEntry;
    TTEntry* first_entry(Key key) const {
        // Anahtarın üst bitleriyle kova seç (mul-hi), alt 16 bit kayıt anahtarı
        return &table[(__uint128_t(key) * __uint128_t(clusterCount)) >> 64].entry[0];
    }

    Cluster* table = nullptr;
    size_t clusterCount = 0;
    uint8_t generation8 = 0;
};

extern TranspositionTable TT;

}  // namespace mai
