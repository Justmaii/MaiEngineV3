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

// Tehdit: "a taşı (from'da) d taşına (to'da) saldırıyor" (NNUE FullThreats girdisi).
// Şahlar ne saldıran ne hedef sayılır ama ışınları keser. Ham kodlama:
// saldıran | hedef<<4 | kaynak<<8 | hedef kare<<14, bit 31 = eklendi (yoksa kalktı)
constexpr uint32_t ThreatAdded = 1u << 31;
inline uint32_t pack_threat(Piece a, Square from, Square to, Piece d) {
    return uint32_t(a) | uint32_t(d) << 4 | uint32_t(from) << 8 | uint32_t(to) << 14;
}

// Bir hamlenin tehditlerde yaptığı değişiklik (NNUE'nin artımlı güncellemesi için),
// ve hamleden sonraki piyonlar ile şah kareleri. Değişiklikler sırayla, tahtanın o
// anki haline göre yazılır; aynı tehdit hem kalkıp hem eklenebilir (birbirini götürür).
constexpr int MaxThreatChanges = 128;
struct ThreatDelta {
    int count;
    bool overflow;  // sığmadıysa NNUE sıfırdan hesaplar
    uint32_t raw[MaxThreatChanges];
    Bitboard pawns[COLOR_NB];
    Square ksq[COLOR_NB];
    ThreatDelta() {}  // bilerek sıfırlanmaz: her hamlede doldurulur
};

// Hamleyle geri alınamayan bilgiler; her hamlede yığına bir kopya eklenir.
struct StateInfo {
    DirtyPiece dirty;
    Key key;
    Key pawnKey;           // piyonlar (correction ve pawn history)
    Key minorPieceKey;     // atlar ve filler
    Key nonPawnKey[COLOR_NB];  // piyon olmayan taşlar (şah dahil), renk başına
    int castlingRights;
    int rule50;
    int pliesFromNull;
    int repetition;  // aynı pozisyonun önceki geçişine uzaklık (3 kez ise negatif), yoksa 0
    Square epSquare;
    Piece captured;
    Bitboard checkers;  // hamle sırası kimdeyse onun şahına şah çeken taşlar
    // Şah bilgisi (gives_check ve SEE için)
    Bitboard blockersForKing[COLOR_NB];  // c renginin şahıyla bir kaydıran taş arasında tek başına duran taşlar
    Bitboard pinners[COLOR_NB];          // c renginin, rakip şaha karşı açmaz yapan taşları
    Bitboard checkSquares[PIECE_TYPE_NB];  // sırası gelen taraf bu tipte taşı buraya koyarsa şah çeker
};

// Taş değerleri (Stockfish 19 ölçeği: 208 = 1 piyon). SEE, budama eşikleri ve
// taş materyali (non_pawn_material) bunları kullanır.
constexpr Value PawnValue = 208, KnightValue = 781, BishopValue = 825, RookValue = 1276, QueenValue = 2538;
constexpr Value PieceValue[PIECE_NB] = {0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0,
                                        0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0};

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
    const ThreatDelta& threat_delta(int idx) const { return deltas[idx]; }
    // Tahtadaki tüm tehditler (ham kodlama, sırasız); en fazla max tane yazar, sayıyı döner
    int all_threats(uint32_t* out, int max) const;
    Key key() const { return state().key; }
    Key pawn_key() const { return state().pawnKey; }
    Key minor_piece_key() const { return state().minorPieceKey; }
    Key non_pawn_key(Color c) const { return state().nonPawnKey[c]; }
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
        return KnightValue * popcount(pieces(c, KNIGHT)) + BishopValue * popcount(pieces(c, BISHOP))
             + RookValue * popcount(pieces(c, ROOK)) + QueenValue * popcount(pieces(c, QUEEN));
    }
    // c renginin pt tipindeki taşlarının saldırdığı kareler
    template <PieceType Pt>
    Bitboard attacks_by(Color c) const;
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
    // Tek hamlede tekrara gidilebiliyor mu? (cuckoo tabloları, Stockfish'teki gibi)
    bool upcoming_repetition(int ply) const;
    static void init_cuckoo();

    // Hata ayıklama: anahtarları ve bitboard'ları sıfırdan hesaplayıp karşılaştırır
    bool pos_is_ok(std::string* why = nullptr) const;
    Key compute_key() const;
    Key compute_pawn_key() const;
    void compute_piece_keys(StateInfo& st) const;

private:
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    // Tehdit değişikliklerini de yazan sürümler (do_move için)
    void put_piece(Piece pc, Square s, ThreatDelta& d);
    void remove_piece(Square s, ThreatDelta& d);
    void move_piece(Square from, Square to, ThreatDelta& d);
    void threats_of(Piece pc, Square s, uint32_t added, ThreatDelta& d) const;
    void rays_through(Square s, uint32_t added, ThreatDelta& d) const;
    ThreatDelta& new_delta();
    void set_check_info(StateInfo& st) const;
    void update_slider_blockers(StateInfo& st, Color c) const;

    Piece board[SQUARE_NB];
    Bitboard byTypeBB[PIECE_TYPE_NB];
    Bitboard byColorBB[COLOR_NB];
    int castlingRightsMask[SQUARE_NB];
    Color sideToMove;
    int gamePly;
    std::vector<StateInfo> states;
    std::vector<ThreatDelta> deltas;  // states ile aynı indeks
};

std::string square_to_string(Square s);
std::string move_to_uci(Move m);
// UCI metnini (ör. "e2e4", "e7e8q") o pozisyondaki legal hamleye çevirir; yoksa Move::none()
Move uci_to_move(const Position& pos, const std::string& str);

}  // namespace mai
