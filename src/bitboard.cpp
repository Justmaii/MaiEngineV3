// MaiEngine v3 — saldırı tabloları ve magic bitboard üretimi
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "bitboard.h"

#include <cstdlib>
#include <iostream>

#include "misc.h"

namespace mai {

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];
Magic RookMagics[SQUARE_NB];
Magic BishopMagics[SQUARE_NB];

namespace {

Bitboard RookTable[0x19000];   // 102400 kayıt
Bitboard BishopTable[0x1480];  // 5248 kayıt

// Kaydırarak (yavaş) kayma saldırısı — sadece tablo kurarken kullanılır
Bitboard sliding_attack(PieceType pt, Square sq, Bitboard occ) {
    static const int rookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const int bishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const auto& dirs = pt == ROOK ? rookDirs : bishopDirs;
    Bitboard att = 0;
    for (auto& d : dirs) {
        int f = file_of(sq), r = rank_of(sq);
        while (true) {
            f += d[0];
            r += d[1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            Square s = make_square(File(f), Rank(r));
            att |= square_bb(s);
            if (occ & square_bb(s)) break;
        }
    }
    return att;
}

Bitboard step_attacks(Square sq, const int (*steps)[2], int n) {
    Bitboard b = 0;
    for (int i = 0; i < n; ++i) {
        int f = file_of(sq) + steps[i][0], r = rank_of(sq) + steps[i][1];
        if (f >= 0 && f <= 7 && r >= 0 && r <= 7) b |= square_bb(make_square(File(f), Rank(r)));
    }
    return b;
}

// Magic sayıları başlangıçta sabit tohumla aranır (birkaç ms), böylece
// ezber tablo taşımak gerekmez ve sonuç her makinede aynıdır.
void init_magics(PieceType pt, Bitboard table[], Magic magics[]) {
    Bitboard occupancy[4096], reference[4096];
    int epoch[4096] = {}, cnt = 0;
    PRNG rng(728);
    Bitboard* next = table;

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        // Kenarlar maskeye girmez (kendi hattı hariç)
        Bitboard edges = ((Rank1BB | Rank8BB) & ~rank_bb(rank_of(s)))
                       | ((FileABB | FileHBB) & ~file_bb(file_of(s)));
        Magic& m = magics[s];
        m.mask = sliding_attack(pt, s, 0) & ~edges;
        m.shift = 64 - popcount(m.mask);
        m.attacks = next;

        // Carry-Rippler: maskenin tüm alt kümeleri
        int size = 0;
        Bitboard b = 0;
        do {
            occupancy[size] = b;
            reference[size] = sliding_attack(pt, s, b);
            ++size;
            b = (b - m.mask) & m.mask;
        } while (b);
        next += size;

        for (int i = 0; i < size;) {
            for (m.magic = 0; popcount((m.magic * m.mask) >> 56) < 6;)
                m.magic = rng.sparse_rand();
            ++cnt;
            for (i = 0; i < size; ++i) {
                unsigned idx = m.index(occupancy[i]);
                if (epoch[idx] < cnt) {
                    epoch[idx] = cnt;
                    m.attacks[idx] = reference[i];
                } else if (m.attacks[idx] != reference[i])
                    break;
            }
        }
    }
}

}  // namespace

void Bitboards::init() {
    static const int knightSteps[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2},
                                          {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    static const int kingSteps[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                        {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    static const int wPawn[2][2] = {{-1, 1}, {1, 1}};
    static const int bPawn[2][2] = {{-1, -1}, {1, -1}};

    init_magics(ROOK, RookTable, RookMagics);
    init_magics(BISHOP, BishopTable, BishopMagics);

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        PawnAttacks[WHITE][s] = step_attacks(s, wPawn, 2);
        PawnAttacks[BLACK][s] = step_attacks(s, bPawn, 2);
        PseudoAttacks[KNIGHT][s] = step_attacks(s, knightSteps, 8);
        PseudoAttacks[KING][s] = step_attacks(s, kingSteps, 8);
        PseudoAttacks[BISHOP][s] = attacks_bb<BISHOP>(s, 0);
        PseudoAttacks[ROOK][s] = attacks_bb<ROOK>(s, 0);
        PseudoAttacks[QUEEN][s] = PseudoAttacks[BISHOP][s] | PseudoAttacks[ROOK][s];
    }

    for (Square a = SQ_A1; a <= SQ_H8; ++a)
        for (Square b = SQ_A1; b <= SQ_H8; ++b) {
            BetweenBB[a][b] = LineBB[a][b] = 0;
            for (PieceType pt : {BISHOP, ROOK}) {
                if (a != b && (PseudoAttacks[pt][a] & square_bb(b))) {
                    LineBB[a][b] = (attacks_bb(pt, a, 0) & attacks_bb(pt, b, 0)) | square_bb(a) | square_bb(b);
                    BetweenBB[a][b] = attacks_bb(pt, a, square_bb(b)) & attacks_bb(pt, b, square_bb(a));
                }
            }
        }
}

std::string Bitboards::pretty(Bitboard b) {
    std::string s = "+---+---+---+---+---+---+---+---+\n";
    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8; ++f)
            s += (b & square_bb(make_square(File(f), Rank(r)))) ? "| X " : "|   ";
        s += "| " + std::to_string(r + 1) + "\n+---+---+---+---+---+---+---+---+\n";
    }
    return s + "  a   b   c   d   e   f   g   h\n";
}

}  // namespace mai
