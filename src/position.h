// MaiEngine v3 — pozisyon (bitboard tahta)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <string>
#include <vector>

#include "bitboard.h"
#include "types.h"

namespace mai {

namespace Zobrist {
extern Key psq[PIECE_NB][SQUARE_NB];
extern Key enpassant[8];
extern Key castling[CASTLING_RIGHT_NB];
extern Key side;
void init();
}  // namespace Zobrist

// Hamleyle geri alınamayan bilgiler; her hamlede yığına bir kopya eklenir.
struct StateInfo {
    Key key;
    Key pawnKey;
    int castlingRights;
    int rule50;
    int pliesFromNull;
    Square epSquare;
    Piece captured;
    Bitboard checkers;  // hamle sırası kimdeyse onun şahına şah çeken taşlar
};

class Position {
public:
    static constexpr const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    Position() { set(StartFEN); }
    bool set(const std::string& fen);
    std::string fen() const;
    std::string pretty() const;

    // Taşlar
    Bitboard pieces() const { return byTypeBB[ALL_PIECES]; }
    Bitboard pieces(PieceType pt) const { return byTypeBB[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byTypeBB[a] | byTypeBB[b]; }
    Bitboard pieces(Color c) const { return byColorBB[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard pieces(Color c, PieceType a, PieceType b) const { return byColorBB[c] & (byTypeBB[a] | byTypeBB[b]); }
    Piece piece_on(Square s) const { return board[s]; }
    bool empty(Square s) const { return board[s] == NO_PIECE; }
    Square king_square(Color c) const { return lsb(pieces(c, KING)); }
    Piece moved_piece(Move m) const { return board[m.from_sq()]; }

    // Durum
    Color side_to_move() const { return sideToMove; }
    const StateInfo& state() const { return states.back(); }
    Key key() const { return state().key; }
    Key pawn_key() const { return state().pawnKey; }
    Bitboard checkers() const { return state().checkers; }
    int castling_rights() const { return state().castlingRights; }
    Square ep_square() const { return state().epSquare; }
    int rule50() const { return state().rule50; }
    int game_ply() const { return gamePly; }
    Piece captured_piece() const { return state().captured; }

    // Saldırılar
    Bitboard attackers_to(Square s, Bitboard occ) const;
    Bitboard attackers_to(Square s) const { return attackers_to(s, pieces()); }

    bool capture(Move m) const {
        return (!empty(m.to_sq()) && m.type_of() != CASTLING) || m.type_of() == EN_PASSANT;
    }

    // Hamle yapma / geri alma (hamlenin legal olduğu varsayılır)
    void do_move(Move m);
    void undo_move(Move m);
    void do_null_move();
    void undo_null_move();

    // Tekrar / 50 hamle beraberliği (ply: aramadaki derinlik, kökten itibaren)
    bool is_draw(int ply) const;

    // Hata ayıklama: anahtarları ve bitboard'ları sıfırdan hesaplayıp karşılaştırır
    bool pos_is_ok(std::string* why = nullptr) const;
    Key compute_key() const;
    Key compute_pawn_key() const;

private:
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    void set_castling_right(Color c, bool kingSide);

    Piece board[SQUARE_NB];
    Bitboard byTypeBB[PIECE_TYPE_NB];
    Bitboard byColorBB[COLOR_NB];
    int castlingRightsMask[SQUARE_NB];
    Color sideToMove;
    int gamePly;
    std::vector<StateInfo> states;
};

std::string square_to_string(Square s);
std::string move_to_uci(Move m);
// UCI metnini (ör. "e2e4", "e7e8q") o pozisyondaki legal hamleye çevirir; yoksa Move::none()
Move uci_to_move(const Position& pos, const std::string& str);

}  // namespace mai
