// MaiEngine v3 — legal hamle üretimi (pin/şah maskeleri)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Fikir: hamleleri üretirken yasallığı da maskelerle sağla, sonradan
// "oyna-kontrol et-geri al" yapma.
//   danger    : rakibin saldırdığı kareler (bizim şah tahtadan kaldırılmış
//               olarak — yoksa şah, kalenin hattında geri çekilebilir sanılır)
//   checkmask : tek şah varsa şah çeken taş + aradaki kareler; şah yoksa tüm tahta
//   pinHV/pinD: yatay-dikey / çapraz açmazdaki taşların hareket edebileceği
//               hat (şah ile açmaz yapan taş arası + o taş)
// Çifte şahta sadece şah oynar. En passant çok nadir ve özel (iki piyon aynı
// anda yataydan kalkar), onu tahtada deneyerek kontrol ediyoruz.
#include "movegen.h"

namespace mai {

namespace {

template <GenType T>
inline ExtMove* make_promotions(ExtMove* list, Square from, Square to, bool isCapture) {
    // Alışlı terfilerin hepsi CAPTURES'da; alışsızlarda vezir CAPTURES'da, küçükler QUIETS'te
    if constexpr (T == CAPTURES || T == LEGAL)
        (list++)->move = Move::make<PROMOTION>(from, to, QUEEN);
    if (T == LEGAL || (T == CAPTURES && isCapture) || (T == QUIETS && !isCapture)) {
        (list++)->move = Move::make<PROMOTION>(from, to, KNIGHT);
        (list++)->move = Move::make<PROMOTION>(from, to, ROOK);
        (list++)->move = Move::make<PROMOTION>(from, to, BISHOP);
    }
    return list;
}

template <Color Us>
Bitboard king_danger(const Position& pos, Square ksq) {
    constexpr Color Them = ~Us;
    const Bitboard occ = pos.pieces() ^ square_bb(ksq);
    Bitboard danger = pawn_attacks_bb<Them>(pos.pieces(Them, PAWN))
                    | attacks_bb<KING>(pos.king_square(Them));
    for (Bitboard b = pos.pieces(Them, KNIGHT); b;) danger |= attacks_bb<KNIGHT>(pop_lsb(b));
    for (Bitboard b = pos.pieces(Them, BISHOP, QUEEN); b;) danger |= attacks_bb<BISHOP>(pop_lsb(b), occ);
    for (Bitboard b = pos.pieces(Them, ROOK, QUEEN); b;) danger |= attacks_bb<ROOK>(pop_lsb(b), occ);
    return danger;
}

template <Color Us, GenType T>
ExtMove* generate_all(const Position& pos, ExtMove* list) {
    constexpr Color Them = ~Us;
    constexpr Direction Up = pawn_push(Us);
    constexpr Direction UpRight = Us == WHITE ? NORTH_EAST : SOUTH_WEST;
    constexpr Direction UpLeft = Us == WHITE ? NORTH_WEST : SOUTH_EAST;
    constexpr Bitboard Rank3 = Us == WHITE ? Rank3BB : Rank6BB;
    constexpr Bitboard Rank8 = Us == WHITE ? Rank8BB : Rank1BB;

    const Square ksq = pos.king_square(Us);
    const Bitboard occ = pos.pieces();
    const Bitboard ours = pos.pieces(Us);
    const Bitboard theirs = pos.pieces(Them);
    const Bitboard checkers = pos.checkers();
    const Bitboard danger = king_danger<Us>(pos, ksq);

    // Hedef filtresi (üretim tipine göre)
    const Bitboard typeMask = T == CAPTURES ? theirs : T == QUIETS ? ~occ : ~ours;

    // --- Şah hamleleri ---
    for (Bitboard b = attacks_bb<KING>(ksq) & ~ours & ~danger & typeMask; b;)
        (list++)->move = Move(ksq, pop_lsb(b));

    if (more_than_one(checkers)) return list;  // çifte şah: sadece şah kaçar

    const Bitboard checkmask = checkers ? between_bb(ksq, lsb(checkers)) | checkers : ~0ULL;

    // --- Açmazlar ---
    Bitboard pinHV = 0, pinD = 0;
    for (Bitboard s = attacks_bb<ROOK>(ksq, theirs) & pos.pieces(Them, ROOK, QUEEN); s;) {
        Square sn = pop_lsb(s);
        Bitboard between = between_bb(ksq, sn) & occ;
        if (between && !more_than_one(between)) pinHV |= between_bb(ksq, sn) | square_bb(sn);
    }
    for (Bitboard s = attacks_bb<BISHOP>(ksq, theirs) & pos.pieces(Them, BISHOP, QUEEN); s;) {
        Square sn = pop_lsb(s);
        Bitboard between = between_bb(ksq, sn) & occ;
        if (between && !more_than_one(between)) pinD |= between_bb(ksq, sn) | square_bb(sn);
    }

    const Bitboard targets = checkmask & typeMask;

    // --- At (açmazdaki at hiç oynayamaz) ---
    for (Bitboard b = pos.pieces(Us, KNIGHT) & ~(pinHV | pinD); b;) {
        Square from = pop_lsb(b);
        for (Bitboard t = attacks_bb<KNIGHT>(from) & targets; t;) (list++)->move = Move(from, pop_lsb(t));
    }

    // --- Çapraz gidenler (fil + vezir) ---
    const Bitboard diag = pos.pieces(Us, BISHOP, QUEEN) & ~pinHV;
    for (Bitboard b = diag & ~pinD; b;) {
        Square from = pop_lsb(b);
        for (Bitboard t = attacks_bb<BISHOP>(from, occ) & targets; t;) (list++)->move = Move(from, pop_lsb(t));
    }
    for (Bitboard b = diag & pinD; b;) {
        Square from = pop_lsb(b);
        for (Bitboard t = attacks_bb<BISHOP>(from, occ) & targets & pinD; t;) (list++)->move = Move(from, pop_lsb(t));
    }

    // --- Düz gidenler (kale + vezir) ---
    const Bitboard ortho = pos.pieces(Us, ROOK, QUEEN) & ~pinD;
    for (Bitboard b = ortho & ~pinHV; b;) {
        Square from = pop_lsb(b);
        for (Bitboard t = attacks_bb<ROOK>(from, occ) & targets; t;) (list++)->move = Move(from, pop_lsb(t));
    }
    for (Bitboard b = ortho & pinHV; b;) {
        Square from = pop_lsb(b);
        for (Bitboard t = attacks_bb<ROOK>(from, occ) & targets & pinHV; t;) (list++)->move = Move(from, pop_lsb(t));
    }

    // --- Piyonlar ---
    // Not: iki farklı açmaz hattı sadece şahın karesinde kesişir, bu yüzden
    // "hedef kare pin maskesinde mi" kontrolü piyonun kendi hattında kaldığını garanti eder.
    const Bitboard pawns = pos.pieces(Us, PAWN);
    const Bitboard emptySq = ~occ;

    // İtişler: çapraz açmazdaki piyon itemez, dikey açmazdaki kendi hattında itebilir
    const Bitboard pushers = pawns & ~pinD;
    Bitboard single = (shift<Up>(pushers & ~pinHV) | (shift<Up>(pushers & pinHV) & pinHV)) & emptySq;
    Bitboard dbl = shift<Up>(single & Rank3) & emptySq & checkmask;
    single &= checkmask;

    if constexpr (T != CAPTURES) {
        for (Bitboard b = single & ~Rank8; b;) {
            Square to = pop_lsb(b);
            (list++)->move = Move(to - Up, to);
        }
        for (Bitboard b = dbl; b;) {
            Square to = pop_lsb(b);
            (list++)->move = Move(to - Up - Up, to);
        }
    }
    for (Bitboard b = single & Rank8; b;) {
        Square to = pop_lsb(b);
        list = make_promotions<T>(list, to - Up, to, false);
    }

    // Alışlar: yatay-dikey açmazdaki piyon alamaz, çapraz açmazdaki kendi hattında alabilir
    if constexpr (T != QUIETS) {
        const Bitboard capturers = pawns & ~pinHV;
        const Bitboard freeCap = capturers & ~pinD, pinnedCap = capturers & pinD;
        const Bitboard capTargets = theirs & checkmask;
        Bitboard capR = (shift<UpRight>(freeCap) | (shift<UpRight>(pinnedCap) & pinD)) & capTargets;
        Bitboard capL = (shift<UpLeft>(freeCap) | (shift<UpLeft>(pinnedCap) & pinD)) & capTargets;

        for (Bitboard b = capR & ~Rank8; b;) { Square to = pop_lsb(b); (list++)->move = Move(to - UpRight, to); }
        for (Bitboard b = capL & ~Rank8; b;) { Square to = pop_lsb(b); (list++)->move = Move(to - UpLeft, to); }
        for (Bitboard b = capR & Rank8; b;) { Square to = pop_lsb(b); list = make_promotions<T>(list, to - UpRight, to, true); }
        for (Bitboard b = capL & Rank8; b;) { Square to = pop_lsb(b); list = make_promotions<T>(list, to - UpLeft, to, true); }

        // En passant: tahtada dene (yatay açmaz tuzağı: K..Pp..r aynı sırada)
        const Square ep = pos.ep_square();
        if (ep != SQ_NONE) {
            const Square capsq = ep - Up;
            for (Bitboard b = capturers & pawn_attacks(Them, ep); b;) {
                Square from = pop_lsb(b);
                Bitboard occ2 = (occ ^ square_bb(from) ^ square_bb(capsq)) | square_bb(ep);
                Bitboard att = (attacks_bb<ROOK>(ksq, occ2) & pos.pieces(Them, ROOK, QUEEN))
                             | (attacks_bb<BISHOP>(ksq, occ2) & pos.pieces(Them, BISHOP, QUEEN))
                             | (attacks_bb<KNIGHT>(ksq) & pos.pieces(Them, KNIGHT))
                             | (pawn_attacks(Us, ksq) & pos.pieces(Them, PAWN) & ~square_bb(capsq));
                if (!att) (list++)->move = Move::make<EN_PASSANT>(from, ep);
            }
        }
    }

    // --- Rok (şahta değilken; geçilen kareler "danger" haritasına bakılır —
    // şah yerindeyken şah yokken farkı sadece şahın kendi hattında olur, o da zaten şah demek) ---
    if constexpr (T != CAPTURES) {
        const int cr = pos.castling_rights();
        if (!checkers && cr) {
            constexpr int OO = Us == WHITE ? WHITE_OO : BLACK_OO;
            constexpr int OOO = Us == WHITE ? WHITE_OOO : BLACK_OOO;
            constexpr Square F = relative_square(Us, SQ_F1), G = relative_square(Us, SQ_G1);
            constexpr Square D = relative_square(Us, SQ_D1), C = relative_square(Us, SQ_C1);
            constexpr Square B = relative_square(Us, SQ_B1);
            if ((cr & OO) && !(occ & (square_bb(F) | square_bb(G))) && !(danger & (square_bb(F) | square_bb(G))))
                (list++)->move = Move::make<CASTLING>(ksq, G);
            if ((cr & OOO) && !(occ & (square_bb(D) | square_bb(C) | square_bb(B)))
                && !(danger & (square_bb(D) | square_bb(C))))
                (list++)->move = Move::make<CASTLING>(ksq, C);
        }
    }

    return list;
}

}  // namespace

template <GenType T>
ExtMove* generate(const Position& pos, ExtMove* list) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, T>(pos, list) : generate_all<BLACK, T>(pos, list);
}

template ExtMove* generate<CAPTURES>(const Position&, ExtMove*);
template ExtMove* generate<QUIETS>(const Position&, ExtMove*);
template ExtMove* generate<LEGAL>(const Position&, ExtMove*);

}  // namespace mai
