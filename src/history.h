// MaiEngine v3 — hamle sıralama istatistikleri (history tabloları)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "types.h"

namespace mai {

// "Yerçekimli" güncelleme: değer +-D sınırına yaklaştıkça bonusun etkisi azalır,
// böylece tablo doymaz ve yeni bilgi eskisini yavaş yavaş ezer.
template <int D>
struct StatEntry {
    int16_t v = 0;
    operator int() const { return v; }
    void operator<<(int bonus) {
        bonus = std::clamp(bonus, -D, D);
        v = int16_t(v + bonus - v * std::abs(bonus) / D);
    }
    void operator=(int x) { v = int16_t(x); }
};

inline int from_to(Move m) { return m.raw() & 0xFFF; }

// [renk][kaynak-hedef]
struct ButterflyHistory {
    StatEntry<7183> t[COLOR_NB][64 * 64];
    auto& operator()(Color c, Move m) { return t[c][from_to(m)]; }
    const auto& operator()(Color c, Move m) const { return t[c][from_to(m)]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e = v; }
};

// [taş][hedef] — continuation history'nin bir dilimi
struct PieceToHistory {
    StatEntry<29952> t[PIECE_NB][SQUARE_NB];
    auto& operator()(Piece pc, Square s) { return t[pc][s]; }
    const auto& operator()(Piece pc, Square s) const { return t[pc][s]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e = v; }
};

// [önceki taş][önceki hedef] -> PieceToHistory
struct ContinuationHistory {
    PieceToHistory t[PIECE_NB][SQUARE_NB];
    PieceToHistory& operator()(Piece pc, Square s) { return t[pc][s]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e.fill(v); }
};

// [taş][hedef][alınan tip]
struct CaptureHistory {
    StatEntry<10692> t[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];
    auto& operator()(Piece pc, Square s, PieceType captured) { return t[pc][s][captured]; }
    const auto& operator()(Piece pc, Square s, PieceType captured) const { return t[pc][s][captured]; }
    void fill(int v) { for (auto& a : t) for (auto& b : a) for (auto& e : b) e = v; }
};

// [taş][hedef] -> o hamleyi en iyi cevaplayan hamle
struct CounterMoveHistory {
    Move t[PIECE_NB][SQUARE_NB];
    Move& operator()(Piece pc, Square s) { return t[pc][s]; }
    void clear() { std::memset(static_cast<void*>(t), 0, sizeof(t)); }
};

}  // namespace mai
