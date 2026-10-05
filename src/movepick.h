// MaiEngine v3 — aşamalı hamle sıralayıcı
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include "history.h"
#include "movegen.h"
#include "position.h"

namespace mai {

class MovePicker {
public:
    MovePicker(const MovePicker&) = delete;
    MovePicker& operator=(const MovePicker&) = delete;
    // Ana arama (depth > 0) ve quiescence (depth <= 0); şahtaysa tüm kaçışlar
    MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const LowPlyHistory* lph,
               const CaptureHistory* ch, const PieceToHistory** cont, const PawnHistory* ph, int ply);
    // ProbCut: SEE'si eşiği geçen alışlar
    MovePicker(const Position& p, Move ttm, int threshold, const CaptureHistory* ch);

    Move next_move();
    void skip_quiet_moves() { skipQuiets = true; }

private:
    template <typename Pred>
    Move select(Pred filter);
    template <GenType T>
    ExtMove* score(ExtMove* end);

    const Position& pos;
    const ButterflyHistory* mainHistory = nullptr;
    const LowPlyHistory* lowPlyHistory = nullptr;
    const CaptureHistory* captureHistory = nullptr;
    const PieceToHistory** contHist = nullptr;
    const PawnHistory* pawnHistory = nullptr;
    Move ttMove;
    ExtMove *cur, *endCur, *endBadCaptures, *endCaptures, *endGenerated;
    int stage;
    int threshold = 0;
    Depth depth = 0;
    int ply = 0;
    bool skipQuiets = false;
    ExtMove moves[MAX_MOVES];
};

}  // namespace mai
