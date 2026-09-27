// MaiEngine v3 — transposition table (kovalı, yaşlı)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "tt.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace mai {

TranspositionTable TT;

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev) {
    // Aynı pozisyon için yeni hamle yoksa eskisini koru
    if (m || uint16_t(k) != key16) move16 = m.raw();

    // Değerli bilgiyi silme: tam değer, farklı pozisyon, ya da yeterince derin ise yaz
    if (b == BOUND_EXACT || uint16_t(k) != key16 || d - DEPTH_OFFSET + 2 * pv > depth8 - 4) {
        key16 = uint16_t(k);
        depth8 = uint8_t(d - DEPTH_OFFSET);
        genBound8 = uint8_t(TT.generation8 | uint8_t(pv) << 2 | b);
        value16 = int16_t(v);
        eval16 = int16_t(ev);
    }
}

TranspositionTable::~TranspositionTable() { std::free(table); }

void TranspositionTable::resize(size_t mb) {
    std::free(table);
    clusterCount = mb * 1024 * 1024 / sizeof(Cluster);
    table = static_cast<Cluster*>(std::aligned_alloc(64, ((clusterCount * sizeof(Cluster) + 63) / 64) * 64));
    if (!table) {
        std::cerr << "TT için " << mb << " MB ayrılamadı\n";
        std::exit(EXIT_FAILURE);
    }
    clear();
}

void TranspositionTable::clear() {
    // Büyük tablolarda paralel sıfırla
    const size_t threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    const size_t stride = clusterCount / threads;
    std::vector<std::thread> pool;
    for (size_t i = 0; i < threads; ++i)
        pool.emplace_back([=, this] {
            size_t start = stride * i, len = i + 1 != threads ? stride : clusterCount - start;
            std::memset(static_cast<void*>(&table[start]), 0, len * sizeof(Cluster));
        });
    for (auto& t : pool) t.join();
    generation8 = 0;
}

TTEntry* TranspositionTable::probe(Key key, bool& found) const {
    TTEntry* const tte = first_entry(key);
    const uint16_t key16 = uint16_t(key);

    for (int i = 0; i < ClusterSize; ++i)
        if (tte[i].key16 == key16 || !tte[i].depth8) {
            // Yaşı tazele (sınır ve PV bitlerini koru)
            tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & (GENERATION_DELTA - 1)));
            return found = bool(tte[i].depth8), &tte[i];
        }

    // Yer yok: en değersiz kaydı seç (sığ ve eski olan)
    TTEntry* replace = tte;
    auto worth = [&](const TTEntry& e) {
        return e.depth8 - ((GENERATION_CYCLE + generation8 - e.genBound8) & GENERATION_MASK);
    };
    for (int i = 1; i < ClusterSize; ++i)
        if (worth(*replace) > worth(tte[i])) replace = &tte[i];
    return found = false, replace;
}

int TranspositionTable::hashfull() const {
    int cnt = 0;
    for (int i = 0; i < 1000; ++i)
        for (int j = 0; j < ClusterSize; ++j)
            cnt += table[i].entry[j].depth8 && (table[i].entry[j].genBound8 & GENERATION_MASK) == generation8;
    return cnt / ClusterSize;
}

}  // namespace mai
