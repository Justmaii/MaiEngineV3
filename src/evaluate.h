// MaiEngine v3 — aramanın kullandığı değerlendirme
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "nnue.h"
#include "position.h"
#include "types.h"

namespace mai::Eval {

// NNUE çıktısını aramaya uygun hale getirir (Stockfish 19'daki gibi):
//  - ağın iki parçası (psqt / katmanlar) çok ayrışıyorsa pozisyon "karmaşık":
//    değer biraz kısılır, iyimserlik büyütülür
//  - taş çoksa değer büyür (oyunsonunda aşırı iyimserlik olmasın)
//  - 50 hamle sayacı büyüdükçe değer sıfıra çekilir (kısır döngüden kaçın)
//  - mat / tablo aralığına asla girmez
inline Value evaluate(const Position& pos, NNUE::Evaluator& ev, int optimism = 0) {
    const NNUE::Output out = ev.evaluate(pos);
    int nnue = out.sum();
    const int complexity = std::abs(out.psqt - out.positional);
    optimism += int(optimism * int64_t(complexity) / 476);
    nnue -= int(nnue * int64_t(complexity) / 18236);

    const int material = 534 * popcount(pos.pieces(PAWN)) + pos.non_pawn_material();
    int v = nnue + int((nnue * int64_t(material) + optimism * int64_t(7675)) / 91000);
    v -= v * pos.rule50() / 199;
    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// UCI "cp" birimine çevir: Stockfish 19'un kazanma oranı modeli. 1 piyon avantajı
// = %50 kazanma şansı olacak şekilde, iç birimin karşılığı materyale göre değişir
// (açılışta ~304).
inline int to_cp(Value v, const Position& pos) {
    const int material = popcount(pos.pieces(PAWN)) + 3 * popcount(pos.pieces(KNIGHT))
                       + 3 * popcount(pos.pieces(BISHOP)) + 5 * popcount(pos.pieces(ROOK))
                       + 9 * popcount(pos.pieces(QUEEN));
    const double m = std::clamp(material, 17, 78) / 58.0;
    const double a = (((-142.72052667 * m + 372.35176398) * m - 340.71073572) * m) + 415.23490212;
    return int(std::round(100 * int(v) / a));
}

}  // namespace mai::Eval
