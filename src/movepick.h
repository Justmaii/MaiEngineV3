// MaiEngine v3 — aşamalı hamle sıralayıcı
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include "history.h"
#include "movegen.h"
#include "position.h"

namespace mai {

class MovePicker {
public:
    // Ana arama
    MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* ch,
               const PieceToHistory** cont, Move counter, const Move* killers);
    // Quiescence: sadece alışlar (+ vezire terfi); şahtaysa tüm kaçışlar
    MovePicker(const Position& p, Move ttm, const ButterflyHistory* mh, const CaptureHistory* ch,
               const PieceToHistory** cont);
    // ProbCut: SEE'si eşiği geçen alışlar
    MovePicker(const Position& p, Move ttm, Value threshold, const CaptureHistory* ch);

    Move next_move(bool skipQuiets = false);

private:
    template <GenType T>
    void score();
    ExtMove* begin() { return cur; }
    ExtMove* end() { return endMoves; }

    const Position& pos;
    const ButterflyHistory* mainHistory = nullptr;
    const CaptureHistory* captureHistory = nullptr;
    const PieceToHistory** contHist = nullptr;
    Move ttMove;
    Move refutations[3] = {};  // killer1, killer2, counter
    ExtMove *cur, *endMoves, *endBadCaptures;
    int stage;
    Depth depth = 0;
    Value threshold = 0;
    ExtMove moves[MAX_MOVES];
};

}  // namespace mai
