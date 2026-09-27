// MaiEngine v3 — legal hamle üretimi (pin/şah maskeleri)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include "position.h"
#include "types.h"

namespace mai {

// Üretilen hamlelerin hepsi LEGAL'dir (şahı açıkta bırakan hamle üretilmez).
//  CAPTURES: tüm alışlar (en passant ve alarak terfi dahil) + vezire terfi eden itişler
//  QUIETS  : alış olmayan hamleler (rok dahil) + alışsız küçük terfiler (N/B/R)
//  LEGAL   : hepsi (CAPTURES + QUIETS)
enum GenType { CAPTURES, QUIETS, LEGAL };

struct ExtMove {
    Move move;
    int value;
};

template <GenType T>
ExtMove* generate(const Position& pos, ExtMove* list);

template <GenType T>
struct MoveList {
    explicit MoveList(const Position& pos) : last(generate<T>(pos, moveList)) {}
    const ExtMove* begin() const { return moveList; }
    const ExtMove* end() const { return last; }
    size_t size() const { return size_t(last - moveList); }
    bool contains(Move m) const {
        for (const auto& em : *this)
            if (em.move == m) return true;
        return false;
    }

private:
    ExtMove moveList[MAX_MOVES];
    ExtMove* last;
};

}  // namespace mai
