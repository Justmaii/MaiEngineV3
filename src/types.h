// MaiEngine v3 — temel tipler
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <cassert>
#include <cstdint>
#include <string>

namespace mai {

using Bitboard = uint64_t;
using Key = uint64_t;

constexpr int MAX_MOVES = 256;
constexpr int MAX_PLY = 246;

// Değerler (Stockfish ölçeği: 208 = bir piyon)
using Value = int;
constexpr Value VALUE_ZERO = 0;
constexpr Value VALUE_DRAW = 0;
constexpr Value VALUE_KNOWN_WIN = 10000;
constexpr Value VALUE_MATE = 32000;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_NONE = 32002;
constexpr Value VALUE_TB_WIN_IN_MAX_PLY = VALUE_MATE - 2 * MAX_PLY;
constexpr Value VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_TB_WIN_IN_MAX_PLY;
constexpr Value VALUE_MATE_IN_MAX_PLY = VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;

constexpr Value mate_in(int ply) { return VALUE_MATE - ply; }
constexpr Value mated_in(int ply) { return -VALUE_MATE + ply; }

using Depth = int;
constexpr Depth DEPTH_QS = 0;
constexpr Depth DEPTH_NONE = -6;
constexpr Depth DEPTH_OFFSET = -7;  // TT'de derinlik bu kadar kaydırılarak saklanır

enum Color : int { WHITE, BLACK, COLOR_NB = 2 };
constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int {
    NO_PIECE_TYPE, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    ALL_PIECES = 0, PIECE_TYPE_NB = 8
};

enum Piece : int {
    NO_PIECE,
    W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB = 16
};

constexpr Piece make_piece(Color c, PieceType pt) { return Piece((c << 3) + pt); }
constexpr PieceType type_of(Piece p) { return PieceType(p & 7); }
constexpr Color color_of(Piece p) { return Color(p >> 3); }

// clang-format off
enum Square : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE, SQUARE_NB = 64
};
// clang-format on

enum Direction : int {
    NORTH = 8, EAST = 1, SOUTH = -8, WEST = -1,
    NORTH_EAST = 9, NORTH_WEST = 7, SOUTH_EAST = -7, SOUTH_WEST = -9
};

enum File : int { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H };
enum Rank : int { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8 };

enum CastlingRights : int {
    NO_CASTLING = 0,
    WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8,
    ANY_CASTLING = 15, CASTLING_RIGHT_NB = 16
};

constexpr Square operator+(Square s, Direction d) { return Square(int(s) + int(d)); }
constexpr Square operator-(Square s, Direction d) { return Square(int(s) - int(d)); }
inline Square& operator++(Square& s) { return s = Square(int(s) + 1); }

constexpr Square make_square(File f, Rank r) { return Square((r << 3) + f); }
constexpr File file_of(Square s) { return File(s & 7); }
constexpr Rank rank_of(Square s) { return Rank(s >> 3); }
constexpr bool is_ok(Square s) { return s >= SQ_A1 && s <= SQ_H8; }
// Siyah için tahtayı dikey çevir (a1 <-> a8)
constexpr Square relative_square(Color c, Square s) { return Square(s ^ (c * 56)); }
constexpr Rank relative_rank(Color c, Rank r) { return Rank(r ^ (c * 7)); }
constexpr Direction pawn_push(Color c) { return c == WHITE ? NORTH : SOUTH; }

// Hamle: 16 bit (Stockfish ile aynı yerleşim)
//  bit 0-5  : hedef kare
//  bit 6-11 : kaynak kare
//  bit 12-13: terfi taşı - KNIGHT (0..3)
//  bit 14-15: tip (NORMAL, PROMOTION, EN_PASSANT, CASTLING)
// Rok: şah kendi karesinden iki kare öteye (e1g1) kodlanır.
enum MoveType : int {
    NORMAL = 0, PROMOTION = 1 << 14, EN_PASSANT = 2 << 14, CASTLING = 3 << 14
};

class Move {
public:
    Move() = default;
    constexpr explicit Move(uint16_t d) : data(d) {}
    constexpr Move(Square from, Square to) : data(uint16_t((from << 6) + to)) {}

    template <MoveType T>
    static constexpr Move make(Square from, Square to, PieceType pt = KNIGHT) {
        return Move(uint16_t(T + ((pt - KNIGHT) << 12) + (from << 6) + to));
    }

    constexpr Square from_sq() const { return Square((data >> 6) & 0x3F); }
    constexpr Square to_sq() const { return Square(data & 0x3F); }
    constexpr MoveType type_of() const { return MoveType(data & (3 << 14)); }
    constexpr PieceType promotion_type() const { return PieceType(((data >> 12) & 3) + KNIGHT); }
    constexpr uint16_t raw() const { return data; }

    constexpr bool is_ok() const { return none().data != data && null().data != data; }
    static constexpr Move none() { return Move(0); }
    static constexpr Move null() { return Move(65); }

    constexpr bool operator==(const Move& m) const { return data == m.data; }
    constexpr bool operator!=(const Move& m) const { return data != m.data; }
    constexpr explicit operator bool() const { return data != 0; }

private:
    uint16_t data = 0;
};

}  // namespace mai
