// MaiEngine v3 — NNUE değerlendirmesi (Stockfish 19 ağı)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Ağ: nn-1a298aa575a0.nnue (Stockfish 19, GPLv3). Mimari Stockfish'ten,
// çıkarım kodu bizim. Sonuç Stockfish'in çıktısıyla birebir aynı olmak zorunda
// (bkz. `maiengine nnuecheck`).
//
// Girdiler üç özellik setinin toplamı, hepsi aynı 1024'lük accumulator'a eklenir:
//  - HalfKAv2_hm (şah kovası x taş x kare, int16 ağırlık) — SF15.1'dekiyle aynı
//  - FullThreats (hangi taş hangi taşa saldırıyor, int8 ağırlık)
//  - PP_3Wide (aynı/komşu dikeydeki piyon çiftleri, int8 ağırlık)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "position.h"

namespace mai::NNUE {

constexpr int HalfDims = 1024;
constexpr int PsqDims = 22528;     // 32 şah kovası * 11 taş tipi * 64 kare
constexpr int ThreatDims = 59808;  // FullThreats
constexpr int PairDims = 4560;     // PP_3Wide: 96 piyon kimliği, ikili kombinasyon
constexpr int OtherDims = ThreatDims + PairDims;  // int8 ağırlıklı blok (tehdit + piyon çifti)
constexpr int PsqtBuckets = 8;
constexpr int LayerStacks = 8;
constexpr int L2 = 32;  // fc0 çıkışı (son ikisi farkı doğrudan çıkışa gider)
constexpr int L3 = 32;  // fc1 çıkışı

constexpr int MaxThreats = 256;  // bir pozisyondaki etkin tehdit sayısı sınırı

constexpr const char* DefaultNetName = "nn-1a298aa575a0.nnue";

// Ağı yükler; hata varsa açıklamasını err'e yazar.
bool load(const std::string& path, std::string* err = nullptr);
bool loaded();
bool has_psqt();  // ağın psqt çıkışı var mı (SF19: var, SFNNv17: yok)
const std::string& description();

// Ağın iki parçası, Stockfish'in iç ölçeğinde, sırası gelen tarafın gözünden
struct Output {
    int psqt, positional;
    int sum() const { return psqt + positional; }
};

// Bir pozisyonun iki bakıştan ilk katman toplamı (bias + HalfKA + tehdit + piyon çifti),
// Stockfish 19'daki gibi tek dizide: her güncelleme tek geçişte okunup yazılır.
struct alignas(64) Accumulator {
    int16_t acc[COLOR_NB][HalfDims];
    int32_t psqt[COLOR_NB][PsqtBuckets];
    Key key[COLOR_NB];  // bu kayıt hangi pozisyon için hesaplandı (0 = geçersiz)
};

// Finny tablosu: her (bakış, şah karesi) için en son görülen taş dizilimi ve
// HalfKA toplamı. Şah oynayınca sıfırdan toplamak yerine sadece farkları uygularız.
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
    Output evaluate(const Position& pos);
    void clear();  // Finny tablosunu ve yığını sıfırla (yeni oyun)

    // İstatistik (hız profili için)
    uint64_t evals = 0, refreshes = 0, updates = 0, rows = 0;  // rows: uygulanan int8 satır sayısı

private:
    void update(const Position& pos, int idx, Color persp);
    bool forward(const Position& pos, int k, Color persp);   // k-1 hesaplı -> k
    bool backward(const Position& pos, int k, Color persp);  // k+1 hesaplı -> k
    bool hybrid(const Position& pos, int idx, Color persp);  // şah aynı yarıda oynadı
    void refresh(const Position& pos, int idx, Color persp); // Finny + tüm tehditler

    std::vector<Accumulator> stack;  // pos.state_index() ile indekslenir
    std::vector<FinnyEntry> finny;   // [renk][şah karesi]
};

// Test/karşılaştırma için: accumulator kullanmadan sıfırdan değerlendirme
Output evaluate_scratch(const Position& pos);

}  // namespace mai::NNUE
