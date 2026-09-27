// MaiEngine v3 — aramanın kullandığı değerlendirme
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <algorithm>

#include "nnue.h"
#include "position.h"
#include "types.h"

namespace mai::Eval {

// NNUE çıktısını aramaya uygun hale getirir (Stockfish 15.1'deki gibi):
//  - taş azaldıkça ölçek küçülür (oyunsonunda aşırı iyimserlik olmasın)
//  - 50 hamle sayacı büyüdükçe değer sıfıra çekilir (kısır döngüden kaçın)
//  - mat / tablo aralığına asla girmez
inline Value evaluate(const Position& pos, NNUE::Evaluator& ev) {
    const int nnue = ev.evaluate(pos);
    const int scale = 1064 + 106 * pos.non_pawn_material() / 5120;
    Value v = nnue * scale / 1024;
    v = v * (200 - pos.rule50()) / 214;
    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// UCI "cp" birimine çevir (1 piyon = 208 iç birim)
inline int to_cp(Value v) { return v * 100 / PawnValueEg; }

}  // namespace mai::Eval
