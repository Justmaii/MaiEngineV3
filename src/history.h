// MaiEngine v3 — hamle sıralama ve düzeltme istatistikleri (history tabloları)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Tablolar ve boyutları Stockfish 19'daki gibi (GPLv3). Stockfish bazılarını
// iş parçacıkları arasında paylaşıyor; bizde her iş parçacığının kendisi var.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "position.h"
#include "types.h"

namespace mai {

constexpr int PAWN_HISTORY_SIZE = 8192;  // 2'nin kuvveti olmalı
constexpr int CORRHIST_SIZE = 65536;     // 2'nin kuvveti olmalı
constexpr int CORRECTION_HISTORY_LIMIT = 1024;
constexpr int LOW_PLY_HISTORY_SIZE = 5;

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

// [renk][hamlenin 16 biti] — sessiz hamlelerin genel başarısı
struct ButterflyHistory {
    StatEntry<7183> t[COLOR_NB][65536];
    auto& operator()(Color c, Move m) { return t[c][m.raw()]; }
    const auto& operator()(Color c, Move m) const { return t[c][m.raw()]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e = v; }
};

// [ply][hamlenin 16 biti] — köke yakın hamle sıralaması
struct LowPlyHistory {
    StatEntry<7183> t[LOW_PLY_HISTORY_SIZE][65536];
    auto& operator()(int ply, Move m) { return t[ply][m.raw()]; }
    const auto& operator()(int ply, Move m) const { return t[ply][m.raw()]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e = v; }
};

// [taş][hedef] — continuation / pawn history'nin bir dilimi
template <int D>
struct PieceToStats {
    StatEntry<D> t[PIECE_NB][SQUARE_NB];
    auto& operator()(Piece pc, Square s) { return t[pc][s]; }
    const auto& operator()(Piece pc, Square s) const { return t[pc][s]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e = v; }
};
using PieceToHistory = PieceToStats<30000>;

// [önceki taş][önceki hedef] -> PieceToHistory
template <typename T>
struct PieceToTable {
    T t[PIECE_NB][SQUARE_NB];
    T& operator()(Piece pc, Square s) { return t[pc][s]; }
    void fill(int v) { for (auto& a : t) for (auto& e : a) e.fill(v); }
};
using ContinuationHistory = PieceToTable<PieceToHistory>;

// [taş][hedef][alınan tip]
struct CaptureHistory {
    StatEntry<10692> t[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];
    auto& operator()(Piece pc, Square s, PieceType captured) { return t[pc][s][captured]; }
    const auto& operator()(Piece pc, Square s, PieceType captured) const { return t[pc][s][captured]; }
    void fill(int v) { for (auto& a : t) for (auto& b : a) for (auto& e : b) e = v; }
};

// Piyon yapısına göre [taş][hedef]
struct PawnHistory {
    std::unique_ptr<PieceToStats<8192>[]> t{new PieceToStats<8192>[PAWN_HISTORY_SIZE]};
    auto& operator[](const Position& pos) { return t[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)]; }
    const auto& operator[](const Position& pos) const { return t[pos.pawn_key() & (PAWN_HISTORY_SIZE - 1)]; }
    void fill(int v) { for (int i = 0; i < PAWN_HISTORY_SIZE; ++i) t[i].fill(v); }
};

// Correction history: statik değerlendirme ile arama sonucu arasındaki farkı
// pozisyonun parçalarına (piyonlar, hafif taşlar, piyon olmayan taşlar) göre öğrenir.
// Hepsi tek tabloda, her biri kendi anahtarıyla indekslenir.
struct CorrectionBundle {
    StatEntry<CORRECTION_HISTORY_LIMIT> pawn, minor, nonPawnWhite, nonPawnBlack;
    void fill(int v) {
        pawn = v;
        minor = v;
        nonPawnWhite = v;
        nonPawnBlack = v;
    }
};

struct CorrectionHistory {
    std::unique_ptr<CorrectionBundle[][COLOR_NB]> t{new CorrectionBundle[CORRHIST_SIZE][COLOR_NB]};
    CorrectionBundle* entry(Key k) { return t[k & (CORRHIST_SIZE - 1)]; }
    const CorrectionBundle* entry(Key k) const { return t[k & (CORRHIST_SIZE - 1)]; }
    void fill(int v) {
        for (int i = 0; i < CORRHIST_SIZE; ++i)
            for (auto& b : t[i]) b.fill(v);
    }
};

using PieceToCorrection = PieceToStats<CORRECTION_HISTORY_LIMIT>;
using ContinuationCorrectionHistory = PieceToTable<PieceToCorrection>;

using TTMoveHistory = StatEntry<8192>;

}  // namespace mai
