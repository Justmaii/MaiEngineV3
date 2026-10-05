// MaiEngine v3 — aşamalı hamle sıralayıcı
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Stockfish 19'daki sıralayıcının uyarlaması (GPLv3).
// Sıra (ana arama): TT hamlesi -> iyi alışlar (SEE) -> iyi sessiz hamleler
// (history) -> kötü alışlar -> kötü sessiz hamleler.
// Şahtayken: TT hamlesi -> tüm kaçışlar (alışlar önce).
// Hamle üreticimiz zaten legal ürettiği için burada yasallık kontrolü yok;
// sadece TT hamlesi (başka pozisyondan gelmiş olabilir) doğrulanıyor.
#include "movepick.h"

#include <limits>
#include <utility>

namespace mai {

namespace {

enum Stages {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, QUIET_INIT, GOOD_QUIET, BAD_CAPTURE, BAD_QUIET,
    EVASION_TT, EVASION_INIT, EVASION,
    PROBCUT_TT, PROBCUT_INIT, PROBCUT,
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE
};

// Sadece eşiğin üstündekileri sırala (gerisi zaten muhtemelen kullanılmayacak)
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {
    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit) {
            ExtMove tmp = *p, *q;
            *p = *++sortedEnd;
            for (q = sortedEnd; q != begin && (q - 1)->value < tmp.value; --q) *q = *(q - 1);
            *q = tmp;
        }
}

}  // namespace

MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const LowPlyHistory* lph,
                       const CaptureHistory* ch, const PieceToHistory** cont, const PawnHistory* ph, int pl)
    : pos(p), mainHistory(mh), lowPlyHistory(lph), captureHistory(ch), contHist(cont), pawnHistory(ph), ttMove(ttm),
      depth(d), ply(pl) {
    const bool ttOk = ttm && pos.legal_move(ttm);
    if (!ttOk) ttMove = Move::none();
    if (pos.checkers()) stage = EVASION_TT + !ttOk;
    else stage = (depth > 0 ? MAIN_TT : QSEARCH_TT) + !ttOk;
}

MovePicker::MovePicker(const Position& p, Move ttm, int th, const CaptureHistory* ch)
    : pos(p), captureHistory(ch), ttMove(ttm), threshold(th) {
    const bool ttOk = ttm && pos.capture_stage(ttm) && pos.legal_move(ttm);
    if (!ttOk) ttMove = Move::none();
    stage = PROBCUT_TT + !ttOk;
}

// Hamlelere sıralama puanı ver. Alışlar: alınan taşın değeri + capture history.
// Sessiz hamleler: history tabloları, şah çekme bonusu, tehditten kaçma/girme.
template <GenType T>
ExtMove* MovePicker::score(ExtMove* end) {
    const Color us = pos.side_to_move();

    [[maybe_unused]] Bitboard threatByLesser[KING + 1];
    if constexpr (T == QUIETS) {
        threatByLesser[PAWN] = 0;
        threatByLesser[KNIGHT] = threatByLesser[BISHOP] = pos.attacks_by<PAWN>(~us);
        threatByLesser[ROOK] = pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | threatByLesser[KNIGHT];
        threatByLesser[QUEEN] = pos.attacks_by<ROOK>(~us) | threatByLesser[ROOK];
        threatByLesser[KING] = 0;
    }

    for (ExtMove* m = cur; m < end; ++m) {
        const Move mv = m->move;
        const Square from = mv.from_sq(), to = mv.to_sq();
        const Piece pc = pos.moved_piece(mv);
        const PieceType pt = type_of(pc);
        const Piece captured = pos.piece_on(to);

        if constexpr (T == CAPTURES)
            m->value = (*captureHistory)(pc, to, type_of(captured)) + 7 * PieceValue[captured];
        else if constexpr (T == QUIETS) {
            m->value = 2 * (*mainHistory)(us, mv);
            m->value += 2 * (*pawnHistory)[pos](pc, to);
            m->value += (*contHist[0])(pc, to);
            m->value += (*contHist[1])(pc, to);
            m->value += (*contHist[2])(pc, to);
            m->value += (*contHist[3])(pc, to);
            m->value += (*contHist[5])(pc, to);

            // Şah çeken hamleye bonus
            m->value += ((pos.check_squares(pt) & square_bb(to)) && pos.see_ge(mv, -75)) * 16384;

            // Daha değersiz bir taşın tehdit ettiği kareye girme cezası / oradan kaçma bonusu
            const int v = 20 * (bool(threatByLesser[pt] & square_bb(from)) - bool(threatByLesser[pt] & square_bb(to)));
            m->value += PieceValue[pt] * v;

            if (ply < LOW_PLY_HISTORY_SIZE) m->value += 8 * (*lowPlyHistory)(ply, mv) / (1 + ply);
        } else {  // kaçışlar
            if (pos.capture_stage(mv)) m->value = PieceValue[captured] + (1 << 28);
            else m->value = (*mainHistory)(us, mv) + (*contHist[0])(pc, to);
        }
    }
    return end;
}

// Koşulu sağlayan sıradaki hamle (TT hamlesi zaten verildiği için atlanır)
template <typename Pred>
Move MovePicker::select(Pred filter) {
    for (; cur < endCur; ++cur)
        if (cur->move != ttMove && filter()) return (cur++)->move;
    return Move::none();
}

Move MovePicker::next_move() {
    constexpr int goodQuietThreshold = -14000;
top:
    switch (stage) {
    case MAIN_TT:
    case EVASION_TT:
    case QSEARCH_TT:
    case PROBCUT_TT:
        ++stage;
        return ttMove;

    case CAPTURE_INIT:
    case PROBCUT_INIT:
    case QCAPTURE_INIT:
        cur = endBadCaptures = moves;
        endCur = endCaptures = score<CAPTURES>(generate<CAPTURES>(pos, cur));
        partial_insertion_sort(cur, endCur, std::numeric_limits<int>::min());
        ++stage;
        goto top;

    case GOOD_CAPTURE:
        if (Move m = select([&]() {
                if (pos.see_ge(cur->move, -cur->value / 18)) return true;
                // Kaybettiren alışları başa topla, sonra oynanacaklar
                std::swap(*endBadCaptures++, *cur);
                return false;
            }))
            return m;
        ++stage;
        [[fallthrough]];

    case QUIET_INIT:
        if (!skipQuiets) {
            endCur = endGenerated = score<QUIETS>(generate<QUIETS>(pos, cur));
            partial_insertion_sort(cur, endCur, -3560 * depth);
        } else
            endGenerated = cur;
        ++stage;
        [[fallthrough]];

    case GOOD_QUIET:
        if (!skipQuiets)
            if (Move m = select([&]() { return cur->value > goodQuietThreshold; })) return m;
        cur = moves;
        endCur = endBadCaptures;
        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE:
        if (Move m = select([]() { return true; })) return m;
        cur = endCaptures;
        endCur = endGenerated;
        ++stage;
        [[fallthrough]];

    case BAD_QUIET:
        if (!skipQuiets) return select([&]() { return cur->value <= goodQuietThreshold; });
        return Move::none();

    case EVASION_INIT:
        cur = moves;
        endCur = endGenerated = score<LEGAL>(generate<LEGAL>(pos, cur));
        partial_insertion_sort(cur, endCur, std::numeric_limits<int>::min());
        ++stage;
        [[fallthrough]];

    case EVASION:
    case QCAPTURE:
        return select([]() { return true; });

    case PROBCUT:
        return select([&]() { return pos.see_ge(cur->move, threshold); });
    }
    return Move::none();
}

}  // namespace mai
