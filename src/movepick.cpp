// MaiEngine v3 — aşamalı hamle sıralayıcı
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Sıra (ana arama): TT hamlesi -> iyi alışlar (SEE) -> sessiz hamleler
// (killer / counter önde, sonra history) -> kötü alışlar.
// Şahtayken: TT hamlesi -> tüm kaçışlar (alışlar önce).
// Hamle üreticimiz zaten legal ürettiği için burada yasallık kontrolü yok;
// sadece TT hamlesi (başka pozisyondan gelmiş olabilir) doğrulanıyor.
#include "movepick.h"

#include <limits>

namespace mai {

namespace {

enum Stages {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, QUIET_INIT, QUIET, BAD_CAPTURE,
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

MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* ch,
                       const PieceToHistory** cont, Move counter, const Move* killers)
    : pos(p), mainHistory(mh), captureHistory(ch), contHist(cont), ttMove(ttm), depth(d) {
    refutations[0] = killers[0];
    refutations[1] = killers[1];
    refutations[2] = counter;
    const bool ttOk = ttm && pos.legal_move(ttm);
    stage = (pos.checkers() ? EVASION_TT : MAIN_TT) + !ttOk;
    if (!ttOk) ttMove = Move::none();
}

MovePicker::MovePicker(const Position& p, Move ttm, const ButterflyHistory* mh, const CaptureHistory* ch,
                       const PieceToHistory** cont)
    : pos(p), mainHistory(mh), captureHistory(ch), contHist(cont), ttMove(ttm) {
    const bool inCheck = pos.checkers();
    const bool ttOk = ttm && (inCheck || pos.capture_stage(ttm)) && pos.legal_move(ttm);
    stage = (inCheck ? EVASION_TT : QSEARCH_TT) + !ttOk;
    if (!ttOk) ttMove = Move::none();
}

MovePicker::MovePicker(const Position& p, Move ttm, Value th, const CaptureHistory* ch)
    : pos(p), captureHistory(ch), ttMove(ttm), threshold(th) {
    const bool ttOk = ttm && pos.capture(ttm) && pos.legal_move(ttm) && pos.see_ge(ttm, threshold);
    stage = PROBCUT_TT + !ttOk;
    if (!ttOk) ttMove = Move::none();
}

template <GenType T>
void MovePicker::score() {
    [[maybe_unused]] Bitboard threatenedByPawn = 0, threatenedByMinor = 0, threatenedByRook = 0, threatened = 0;
    if constexpr (T == QUIETS) {
        const Color us = pos.side_to_move(), them = ~us;
        threatenedByPawn = them == WHITE ? pawn_attacks_bb<WHITE>(pos.pieces(them, PAWN))
                                         : pawn_attacks_bb<BLACK>(pos.pieces(them, PAWN));
        Bitboard minor = threatenedByPawn;
        for (Bitboard b = pos.pieces(them, KNIGHT); b;) minor |= attacks_bb<KNIGHT>(pop_lsb(b));
        for (Bitboard b = pos.pieces(them, BISHOP); b;) minor |= attacks_bb<BISHOP>(pop_lsb(b), pos.pieces());
        threatenedByMinor = minor;
        Bitboard rook = minor;
        for (Bitboard b = pos.pieces(them, ROOK); b;) rook |= attacks_bb<ROOK>(pop_lsb(b), pos.pieces());
        threatenedByRook = rook;
        // Tehdit altındaki kendi taşlarımız: kaçan hamleye bonus
        threatened = (pos.pieces(us, QUEEN) & threatenedByRook) | (pos.pieces(us, ROOK) & threatenedByMinor)
                   | (pos.pieces(us, KNIGHT, BISHOP) & threatenedByPawn);
    }

    for (ExtMove* m = cur; m < endMoves; ++m) {
        const Move mv = m->move;
        const Square from = mv.from_sq(), to = mv.to_sq();
        const Piece pc = pos.moved_piece(mv);
        if constexpr (T == CAPTURES) {
            const Piece captured = mv.type_of() == EN_PASSANT ? make_piece(~pos.side_to_move(), PAWN) : pos.piece_on(to);
            m->value = 6 * PieceValueMg[captured] + (*captureHistory)(pc, to, type_of(captured));
            if (mv.type_of() == PROMOTION) m->value += PieceValueMg[make_piece(WHITE, mv.promotion_type())];
        } else if constexpr (T == QUIETS) {
            const PieceType pt = type_of(pc);
            m->value = 2 * (*mainHistory)(pos.side_to_move(), mv) + 2 * (*contHist[0])(pc, to)
                     + (*contHist[1])(pc, to) + (*contHist[3])(pc, to) + (*contHist[5])(pc, to);
            if (threatened & square_bb(from)) {
                const Bitboard t = square_bb(to);
                m->value += pt == QUEEN && !(t & threatenedByRook)   ? 50000
                          : pt == ROOK && !(t & threatenedByMinor)   ? 25000
                          : !(t & threatenedByPawn)                  ? 15000
                                                                     : 0;
            }
            // Killer'lar ve counter en öne
            if (mv == refutations[0]) m->value += 1 << 27;
            else if (mv == refutations[1]) m->value += 1 << 26;
            else if (mv == refutations[2]) m->value += 1 << 25;
        } else {  // kaçışlar
            if (pos.capture(mv)) {
                const Piece captured = mv.type_of() == EN_PASSANT ? make_piece(~pos.side_to_move(), PAWN) : pos.piece_on(to);
                m->value = PieceValueMg[captured] - int(type_of(pc)) + (1 << 28);
            } else
                m->value = (*mainHistory)(pos.side_to_move(), mv) + 2 * (*contHist[0])(pc, to) - (1 << 28);
        }
    }
}

Move MovePicker::next_move(bool skipQuiets) {
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
        endMoves = generate<CAPTURES>(pos, cur);
        score<CAPTURES>();
        partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
        ++stage;
        goto top;

    case GOOD_CAPTURE:
        while (cur < endMoves) {
            ExtMove em = *cur++;
            if (em.move == ttMove) continue;
            // Kaybettiren alışları sona sakla
            if (pos.see_ge(em.move, -69 * em.value / 1024)) return em.move;
            *endBadCaptures++ = em;
        }
        ++stage;
        [[fallthrough]];

    case QUIET_INIT:
        if (!skipQuiets) {
            cur = endBadCaptures;
            endMoves = generate<QUIETS>(pos, cur);
            score<QUIETS>();
            partial_insertion_sort(cur, endMoves, -3000 * depth);
        } else
            cur = endMoves = endBadCaptures;
        ++stage;
        [[fallthrough]];

    case QUIET:
        if (!skipQuiets)
            while (cur < endMoves) {
                Move m = (cur++)->move;
                if (m != ttMove) return m;
            }
        cur = moves;
        endMoves = endBadCaptures;
        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m != ttMove) return m;
        }
        return Move::none();

    case EVASION_INIT:
        cur = moves;
        endMoves = generate<LEGAL>(pos, cur);
        score<LEGAL>();
        partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
        ++stage;
        [[fallthrough]];

    case EVASION:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m != ttMove) return m;
        }
        return Move::none();

    case PROBCUT:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m != ttMove && pos.capture(m) && pos.see_ge(m, threshold)) return m;
        }
        return Move::none();

    case QCAPTURE:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m != ttMove) return m;
        }
        return Move::none();
    }
    return Move::none();
}

}  // namespace mai
