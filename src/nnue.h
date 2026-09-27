// MaiEngine v3 — NNUE değerlendirmesi (Stockfish 15.1 ağı, HalfKAv2_hm)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Ağ: nn-ad9b42354671.nnue (Stockfish 15.1, GPLv3). Mimari Stockfish'ten,
// çıkarım kodu bizim. Sonuç Stockfish'in çıktısıyla birebir aynı olmak zorunda
// (bkz. `maiengine nnuecheck`).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "position.h"

namespace mai::NNUE {

constexpr int HalfDims = 1024;
constexpr int InputDims = 22528;  // 32 şah kovası * 11 taş tipi * 64 kare
constexpr int PsqtBuckets = 8;
constexpr int LayerStacks = 8;
constexpr int L1Out = 16;  // 15 + 1 doğrudan çıkışa giden
constexpr int L2In = 32;   // 30 gerçek + 2 dolgu
constexpr int L2Out = 32;
constexpr int L3In = 32;

constexpr const char* DefaultNetName = "nn-ad9b42354671.nnue";

// Ağı yükler; hata varsa açıklamasını err'e yazar.
bool load(const std::string& path, std::string* err = nullptr);
bool loaded();
const std::string& description();

// Bir bakış açısının ilk katman toplamları
struct alignas(64) Accumulator {
    int16_t acc[COLOR_NB][HalfDims];
    int32_t psqt[COLOR_NB][PsqtBuckets];
    Key key[COLOR_NB];  // bu kayıt hangi pozisyon için hesaplandı (0 = geçersiz)
};

// Finny tablosu: her (bakış, şah karesi) için en son görülen taş dizilimi ve
// toplamı. Şah oynayınca sıfırdan toplamak yerine sadece farkları uygularız.
struct alignas(64) FinnyEntry {
    int16_t acc[HalfDims];
    int32_t psqt[PsqtBuckets];
    Bitboard byColor[COLOR_NB];
    Bitboard byType[PIECE_TYPE_NB];
};

// Her arama iş parçacığının kendi değerlendiricisi olur.
class Evaluator {
public:
    Evaluator();
    // Sırası gelen tarafın gözünden, Stockfish'in iç ölçeğinde (piyon ~ 208)
    int evaluate(const Position& pos);
    void clear();  // Finny tablosunu ve yığını sıfırla (yeni oyun)

    // İstatistik (hız profili için)
    uint64_t refreshes = 0, updates = 0;

private:
    void update(const Position& pos, int idx, Color persp);
    void refresh(const Position& pos, Accumulator& acc, Color persp);

    std::vector<Accumulator> stack;  // pos.state_index() ile indekslenir
    std::vector<FinnyEntry> finny;   // [renk][şah karesi]
};

// Test/karşılaştırma için: accumulator kullanmadan sıfırdan değerlendirme
int evaluate_scratch(const Position& pos);

}  // namespace mai::NNUE
