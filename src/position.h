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

// Bir hamlede yeri değişen taşlar (NNUE artımlı güncellemesi için).
// from == SQ_NONE: taş eklendi (terfi), to == SQ_NONE: taş kalktı (alınan / terfi eden piyon)
struct DirtyPiece {
    int count;
    Piece piece[3];
    Square from[3];
    Square to[3];
};

// Hamleyle geri alınamayan bilgiler; her hamlede yığına bir kopya eklenir.
struct StateInfo {
    DirtyPiece dirty;
    Key key;
    Key pawnKey;
    int castlingRights;
    int rule50;
    int pliesFromNull;
    Square epSquare;
    Piece captured;
    Bitboard checkers;  // hamle sırası kimdeyse onun şahına şah çeken taşlar
    // Şah bilgisi (gives_check ve SEE için)
    Bitboard blockersForKing[COLOR_NB];  // c renginin şahıyla bir kaydıran taş arasında tek başına duran taşlar
    Bitboard pinners[COLOR_NB];          // c renginin, rakip şaha karşı açmaz yapan taşları
    Bitboard checkSquares[PIECE_TYPE_NB];  // sırası gelen taraf bu tipte taşı buraya koyarsa şah çeker
};

// Taş değerleri (Stockfish 15.1 ölçeği: piyon oyunsonu değeri 208 = 1 piyon)
constexpr int PawnValueMg = 126, PawnValueEg = 208;
constexpr int KnightValueMg = 781, KnightValueEg = 854;
constexpr int BishopValueMg = 825, BishopValueEg = 915;
constexpr int RookValueMg = 1276, RookValueEg = 1380;
constexpr int QueenValueMg = 2538, QueenValueEg = 2682;
constexpr int PieceValueMg[PIECE_NB] = {0, PawnValueMg, KnightValueMg, BishopValueMg, RookValueMg, QueenValueMg, 0, 0,
                                        0, PawnValueMg, KnightValueMg, BishopValueMg, RookValueMg, QueenValueMg, 0, 0};
constexpr int PieceValueEg[PIECE_NB] = {0, PawnValueEg, KnightValueEg, BishopValueEg, RookValueEg, QueenValueEg, 0, 0,
                                        0, PawnValueEg, KnightValueEg, BishopValueEg, RookValueEg, QueenValueEg, 0, 0};

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
    // Durum yığınındaki mutlak indeks (NNUE accumulator yığını bununla eşleşir)
    int state_index() const { return int(states.size()) - 1; }
    const StateInfo& state_at(int idx) const { return states[idx]; }
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
    // Hamle üreticisinin CAPTURES listesine giren hamleler: alışlar + vezire terfi
    bool capture_stage(Move m) const {
        return capture(m) || (m.type_of() == PROMOTION && m.promotion_type() == QUEEN);
    }
    Bitboard blockers_for_king(Color c) const { return state().blockersForKing[c]; }
    Bitboard pinners(Color c) const { return state().pinners[c]; }
    Bitboard check_squares(PieceType pt) const { return state().checkSquares[pt]; }
    int non_pawn_material(Color c) const {
        return KnightValueMg * popcount(pieces(c, KNIGHT)) + BishopValueMg * popcount(pieces(c, BISHOP))
             + RookValueMg * popcount(pieces(c, ROOK)) + QueenValueMg * popcount(pieces(c, QUEEN));
    }
    int non_pawn_material() const { return non_pawn_material(WHITE) + non_pawn_material(BLACK); }

    bool gives_check(Move m) const;
    // Statik alış değerlendirmesi: hamlenin SEE değeri >= threshold mu? (şahın
    // korunan kareye giremeyeceğini bilir)
    bool see_ge(Move m, int threshold = 0) const;
    // TT'den / başka düğümden gelen hamle bu pozisyonda legal mi?
    bool legal_move(Move m) const;

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
    void set_check_info(StateInfo& st) const;
    void update_slider_blockers(StateInfo& st, Color c) const;

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
