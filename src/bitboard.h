// MaiEngine v3 — bitboard yardımcıları ve saldırı tabloları
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <bit>
#include <string>

#include "types.h"

namespace mai {

namespace Bitboards {
void init();
std::string pretty(Bitboard b);
}  // namespace Bitboards

constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = FileABB << 1;
constexpr Bitboard FileGBB = FileABB << 6;
constexpr Bitboard FileHBB = FileABB << 7;
constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << 8;
constexpr Bitboard Rank3BB = Rank1BB << 16;
constexpr Bitboard Rank6BB = Rank1BB << 40;
constexpr Bitboard Rank7BB = Rank1BB << 48;
constexpr Bitboard Rank8BB = Rank1BB << 56;

constexpr Bitboard square_bb(Square s) { return 1ULL << s; }
constexpr Bitboard file_bb(File f) { return FileABB << f; }
constexpr Bitboard rank_bb(Rank r) { return Rank1BB << (8 * r); }

inline int popcount(Bitboard b) { return std::popcount(b); }
inline Square lsb(Bitboard b) { assert(b); return Square(std::countr_zero(b)); }
inline Square msb(Bitboard b) { assert(b); return Square(63 - std::countl_zero(b)); }
inline Square pop_lsb(Bitboard& b) { Square s = lsb(b); b &= b - 1; return s; }
constexpr bool more_than_one(Bitboard b) { return b & (b - 1); }

template <Direction D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH) return b << 8;
    else if constexpr (D == SOUTH) return b >> 8;
    else if constexpr (D == EAST) return (b & ~FileHBB) << 1;
    else if constexpr (D == WEST) return (b & ~FileABB) >> 1;
    else if constexpr (D == NORTH_EAST) return (b & ~FileHBB) << 9;
    else if constexpr (D == NORTH_WEST) return (b & ~FileABB) << 7;
    else if constexpr (D == SOUTH_EAST) return (b & ~FileHBB) >> 7;
    else if constexpr (D == SOUTH_WEST) return (b & ~FileABB) >> 9;
    else return 0;
}

// Bir renkteki tüm piyonların saldırdığı kareler (küme halinde)
template <Color C>
constexpr Bitboard pawn_attacks_bb(Bitboard b) {
    return C == WHITE ? shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b)
                      : shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
}

extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];  // iki kare arası (uçlar hariç), hizalı değilse 0
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];     // iki kareden geçen tam doğru, hizalı değilse 0

// "Fancy" magic bitboard
struct Magic {
    Bitboard mask;
    Bitboard magic;
    Bitboard* attacks;
    unsigned shift;
    unsigned index(Bitboard occ) const { return unsigned(((occ & mask) * magic) >> shift); }
};

extern Magic RookMagics[SQUARE_NB];
extern Magic BishopMagics[SQUARE_NB];

inline Bitboard pawn_attacks(Color c, Square s) { return PawnAttacks[c][s]; }
inline Bitboard between_bb(Square a, Square b) { return BetweenBB[a][b]; }
inline Bitboard line_bb(Square a, Square b) { return LineBB[a][b]; }
inline bool aligned(Square a, Square b, Square c) { return LineBB[a][b] & square_bb(c); }

template <PieceType Pt>
inline Bitboard attacks_bb(Square s) {
    static_assert(Pt != PAWN);
    return PseudoAttacks[Pt][s];
}

template <PieceType Pt>
inline Bitboard attacks_bb(Square s, Bitboard occ) {
    static_assert(Pt != PAWN);
    if constexpr (Pt == BISHOP) return BishopMagics[s].attacks[BishopMagics[s].index(occ)];
    else if constexpr (Pt == ROOK) return RookMagics[s].attacks[RookMagics[s].index(occ)];
    else if constexpr (Pt == QUEEN) return attacks_bb<BISHOP>(s, occ) | attacks_bb<ROOK>(s, occ);
    else return PseudoAttacks[Pt][s];
}

inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occ) {
    switch (pt) {
    case BISHOP: return attacks_bb<BISHOP>(s, occ);
    case ROOK: return attacks_bb<ROOK>(s, occ);
    case QUEEN: return attacks_bb<QUEEN>(s, occ);
    default: return PseudoAttacks[pt][s];
    }
}

}  // namespace mai
