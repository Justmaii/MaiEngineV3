// MaiEngine v3 — pozisyon (bitboard tahta)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "position.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "misc.h"
#include "movegen.h"

namespace mai {

namespace Zobrist {
Key psq[PIECE_NB][SQUARE_NB];
Key enpassant[8];
Key castling[CASTLING_RIGHT_NB];
Key side;

void init() {
    PRNG rng(1070372);
    for (int pc = 0; pc < PIECE_NB; ++pc)
        for (int s = 0; s < SQUARE_NB; ++s)
            psq[pc][s] = (pc == NO_PIECE) ? 0 : rng.rand64();
    for (auto& k : enpassant) k = rng.rand64();
    // Rok anahtarları: her hak bitine bir sayı, kombinasyonlar XOR
    Key bits[4];
    for (auto& k : bits) k = rng.rand64();
    for (int cr = 0; cr < CASTLING_RIGHT_NB; ++cr) {
        castling[cr] = 0;
        for (int i = 0; i < 4; ++i)
            if (cr & (1 << i)) castling[cr] ^= bits[i];
    }
    side = rng.rand64();
}
}  // namespace Zobrist

namespace {
const std::string PieceChars = " PNBRQK  pnbrqk";
}

std::string square_to_string(Square s) {
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string move_to_uci(Move m) {
    if (m == Move::none()) return "(none)";
    if (m == Move::null()) return "0000";
    std::string s = square_to_string(m.from_sq()) + square_to_string(m.to_sq());
    if (m.type_of() == PROMOTION) s += " pnbrqk"[m.promotion_type()];
    return s;
}

Move uci_to_move(const Position& pos, const std::string& str) {
    std::string lower = str;
    if (lower.size() == 5) lower[4] = char(std::tolower(lower[4]));
    for (const auto& em : MoveList<LEGAL>(pos))
        if (move_to_uci(em.move) == lower) return em.move;
    return Move::none();
}

void Position::put_piece(Piece pc, Square s) {
    board[s] = pc;
    Bitboard b = square_bb(s);
    byTypeBB[ALL_PIECES] |= b;
    byTypeBB[type_of(pc)] |= b;
    byColorBB[color_of(pc)] |= b;
}

void Position::remove_piece(Square s) {
    Piece pc = board[s];
    Bitboard b = square_bb(s);
    byTypeBB[ALL_PIECES] ^= b;
    byTypeBB[type_of(pc)] ^= b;
    byColorBB[color_of(pc)] ^= b;
    board[s] = NO_PIECE;
}

void Position::move_piece(Square from, Square to) {
    Piece pc = board[from];
    Bitboard b = square_bb(from) | square_bb(to);
    byTypeBB[ALL_PIECES] ^= b;
    byTypeBB[type_of(pc)] ^= b;
    byColorBB[color_of(pc)] ^= b;
    board[from] = NO_PIECE;
    board[to] = pc;
}

bool Position::set(const std::string& fen) {
    std::memset(board, 0, sizeof(board));
    std::memset(byTypeBB, 0, sizeof(byTypeBB));
    std::memset(byColorBB, 0, sizeof(byColorBB));
    std::memset(castlingRightsMask, 0, sizeof(castlingRightsMask));
    states.clear();
    states.reserve(1024);
    StateInfo st{};
    st.epSquare = SQ_NONE;

    std::istringstream ss(fen);
    std::string placement, stm, castle = "-", ep = "-";
    int rule50 = 0, moveNo = 1;
    ss >> placement >> stm >> castle >> ep >> rule50 >> moveNo;
    if (placement.empty()) return false;

    int f = 0, r = 7;
    for (char c : placement) {
        if (c == '/') { --r; f = 0; }
        else if (c >= '1' && c <= '8') f += c - '0';
        else {
            size_t idx = PieceChars.find(c);
            if (idx == std::string::npos || c == ' ' || f > 7 || r < 0) return false;
            put_piece(Piece(idx), make_square(File(f), Rank(r)));
            ++f;
        }
    }
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return false;

    sideToMove = (stm == "b") ? BLACK : WHITE;

    // Standart satranç: şah e1/e8, kaleler köşelerde
    castlingRightsMask[SQ_E1] = WHITE_OO | WHITE_OOO;
    castlingRightsMask[SQ_H1] = WHITE_OO;
    castlingRightsMask[SQ_A1] = WHITE_OOO;
    castlingRightsMask[SQ_E8] = BLACK_OO | BLACK_OOO;
    castlingRightsMask[SQ_H8] = BLACK_OO;
    castlingRightsMask[SQ_A8] = BLACK_OOO;
    for (char c : castle) {
        // Taşlar yerinde değilse hakkı yok say
        if (c == 'K' && board[SQ_E1] == W_KING && board[SQ_H1] == W_ROOK) st.castlingRights |= WHITE_OO;
        if (c == 'Q' && board[SQ_E1] == W_KING && board[SQ_A1] == W_ROOK) st.castlingRights |= WHITE_OOO;
        if (c == 'k' && board[SQ_E8] == B_KING && board[SQ_H8] == B_ROOK) st.castlingRights |= BLACK_OO;
        if (c == 'q' && board[SQ_E8] == B_KING && board[SQ_A8] == B_ROOK) st.castlingRights |= BLACK_OOO;
    }

    // En passant karesi sadece gerçekten alabilecek bir piyon varsa kaydedilir
    // (do_move ile aynı kural, yoksa aynı pozisyonun anahtarı farklı olur).
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1] == '3' || ep[1] == '6')) {
        Square eps = make_square(File(ep[0] - 'a'), Rank(ep[1] - '1'));
        Color us = sideToMove;
        if (relative_rank(us, rank_of(eps)) == RANK_6
            && (pawn_attacks(~us, eps) & pieces(us, PAWN))
            && (pieces(~us, PAWN) & square_bb(eps - pawn_push(us)))
            && empty(eps) && empty(eps + pawn_push(us)))
            st.epSquare = eps;
    }

    st.rule50 = rule50;
    st.pliesFromNull = 0;
    gamePly = std::max(2 * (moveNo - 1), 0) + (sideToMove == BLACK);
    st.captured = NO_PIECE;
    states.push_back(st);
    states.back().key = compute_key();
    states.back().pawnKey = compute_pawn_key();
    states.back().checkers = attackers_to(king_square(sideToMove)) & pieces(~sideToMove);
    // Hamle sırası olmayan taraf şahta olamaz
    if (attackers_to(king_square(~sideToMove)) & pieces(sideToMove)) return false;
    return true;
}

std::string Position::fen() const {
    std::string s;
    for (int r = 7; r >= 0; --r) {
        int emptyCnt = 0;
        for (int f = 0; f < 8; ++f) {
            Piece pc = board[make_square(File(f), Rank(r))];
            if (pc == NO_PIECE) { ++emptyCnt; continue; }
            if (emptyCnt) { s += char('0' + emptyCnt); emptyCnt = 0; }
            s += PieceChars[pc];
        }
        if (emptyCnt) s += char('0' + emptyCnt);
        if (r) s += '/';
    }
    s += sideToMove == WHITE ? " w " : " b ";
    int cr = castling_rights();
    if (cr & WHITE_OO) s += 'K';
    if (cr & WHITE_OOO) s += 'Q';
    if (cr & BLACK_OO) s += 'k';
    if (cr & BLACK_OOO) s += 'q';
    if (!cr) s += '-';
    s += ' ';
    s += ep_square() == SQ_NONE ? "-" : square_to_string(ep_square());
    s += ' ' + std::to_string(rule50()) + ' ' + std::to_string(1 + (gamePly - (sideToMove == BLACK)) / 2);
    return s;
}

std::string Position::pretty() const {
    std::ostringstream os;
    os << "\n +---+---+---+---+---+---+---+---+\n";
    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8; ++f) os << " | " << PieceChars[board[make_square(File(f), Rank(r))]];
        os << " | " << r + 1 << "\n +---+---+---+---+---+---+---+---+\n";
    }
    os << "   a   b   c   d   e   f   g   h\n\nFen: " << fen() << "\nKey: " << std::hex << std::uppercase
       << key() << std::dec << "\nCheckers:";
    for (Bitboard b = checkers(); b;) os << ' ' << square_to_string(pop_lsb(b));
    os << "\n";
    return os.str();
}

Bitboard Position::attackers_to(Square s, Bitboard occ) const {
    return (pawn_attacks(BLACK, s) & pieces(WHITE, PAWN))
         | (pawn_attacks(WHITE, s) & pieces(BLACK, PAWN))
         | (attacks_bb<KNIGHT>(s) & pieces(KNIGHT))
         | (attacks_bb<ROOK>(s, occ) & pieces(ROOK, QUEEN))
         | (attacks_bb<BISHOP>(s, occ) & pieces(BISHOP, QUEEN))
         | (attacks_bb<KING>(s) & pieces(KING));
}

void Position::do_move(Move m) {
    StateInfo st = states.back();  // kopya (vektör büyürse referans geçersizleşir)
    const Color us = sideToMove, them = ~us;
    const Square from = m.from_sq(), to = m.to_sq();
    const Piece pc = board[from];
    const Piece captured = m.type_of() == EN_PASSANT ? make_piece(them, PAWN)
                         : m.type_of() == CASTLING   ? NO_PIECE
                                                     : board[to];
    assert(color_of(pc) == us);
    assert(captured == NO_PIECE || (color_of(captured) == them && type_of(captured) != KING));

    Key k = st.key ^ Zobrist::side;
    if (st.epSquare != SQ_NONE) {
        k ^= Zobrist::enpassant[file_of(st.epSquare)];
        st.epSquare = SQ_NONE;
    }
    ++st.rule50;
    ++st.pliesFromNull;
    st.captured = captured;

    if (m.type_of() == CASTLING) {
        const bool kingSide = to > from;
        const Square rfrom = relative_square(us, kingSide ? SQ_H1 : SQ_A1);
        const Square rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
        const Piece rook = make_piece(us, ROOK);
        move_piece(from, to);
        move_piece(rfrom, rto);
        k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to] ^ Zobrist::psq[rook][rfrom] ^ Zobrist::psq[rook][rto];
    } else {
        if (captured) {
            Square capsq = m.type_of() == EN_PASSANT ? to - pawn_push(us) : to;
            remove_piece(capsq);
            k ^= Zobrist::psq[captured][capsq];
            if (type_of(captured) == PAWN) st.pawnKey ^= Zobrist::psq[captured][capsq];
            st.rule50 = 0;
        }
        move_piece(from, to);
        k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

        if (type_of(pc) == PAWN) {
            st.rule50 = 0;
            st.pawnKey ^= Zobrist::psq[pc][from];
            if (m.type_of() == PROMOTION) {
                Piece promo = make_piece(us, m.promotion_type());
                remove_piece(to);
                put_piece(promo, to);
                k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promo][to];
            } else {
                st.pawnKey ^= Zobrist::psq[pc][to];
                // Çift adım: rakip piyon alabiliyorsa en passant karesini kaydet
                if ((int(to) ^ int(from)) == 16) {
                    Square eps = to - pawn_push(us);
                    if (pawn_attacks(us, eps) & pieces(them, PAWN)) {
                        st.epSquare = eps;
                        k ^= Zobrist::enpassant[file_of(eps)];
                    }
                }
            }
        }
    }

    // Rok hakları: şah/kale kendi karesinden ayrılırsa ya da kale alınırsa
    if (st.castlingRights && (castlingRightsMask[from] | castlingRightsMask[to])) {
        k ^= Zobrist::castling[st.castlingRights];
        st.castlingRights &= ~(castlingRightsMask[from] | castlingRightsMask[to]);
        k ^= Zobrist::castling[st.castlingRights];
    }

    st.key = k;
    sideToMove = them;
    ++gamePly;
    st.checkers = attackers_to(king_square(them)) & pieces(us);
    states.push_back(st);
}

void Position::undo_move(Move m) {
    sideToMove = ~sideToMove;
    --gamePly;
    const Color us = sideToMove;
    const Square from = m.from_sq(), to = m.to_sq();
    const StateInfo& st = states.back();

    if (m.type_of() == CASTLING) {
        const bool kingSide = to > from;
        move_piece(to, from);
        move_piece(relative_square(us, kingSide ? SQ_F1 : SQ_D1), relative_square(us, kingSide ? SQ_H1 : SQ_A1));
    } else {
        if (m.type_of() == PROMOTION) {
            remove_piece(to);
            put_piece(make_piece(us, PAWN), to);
        }
        move_piece(to, from);
        if (st.captured) {
            Square capsq = m.type_of() == EN_PASSANT ? to - pawn_push(us) : to;
            put_piece(st.captured, capsq);
        }
    }
    states.pop_back();
}

void Position::do_null_move() {
    assert(!checkers());
    StateInfo st = states.back();
    Key k = st.key ^ Zobrist::side;
    if (st.epSquare != SQ_NONE) {
        k ^= Zobrist::enpassant[file_of(st.epSquare)];
        st.epSquare = SQ_NONE;
    }
    st.key = k;
    ++st.rule50;
    st.pliesFromNull = 0;
    st.captured = NO_PIECE;
    st.checkers = 0;
    sideToMove = ~sideToMove;
    ++gamePly;
    states.push_back(st);
}

void Position::undo_null_move() {
    states.pop_back();
    sideToMove = ~sideToMove;
    --gamePly;
}

bool Position::is_draw(int ply) const {
    const StateInfo& st = states.back();
    if (st.rule50 > 99) {
        // 50 hamle kuralı — ama mat varsa mat geçerli
        if (!st.checkers) return true;
        if (MoveList<LEGAL>(*this).size()) return true;
    }
    // Tekrar: arama ağacının içindeki ilk tekrar beraberlik sayılır,
    // kökten önceki geçmişte ise üç kez tekrar gerekir.
    const int n = int(states.size()) - 1;
    const int end = std::min({st.rule50, st.pliesFromNull, n});
    int count = 0;
    for (int i = 4; i <= end; i += 2) {
        if (states[n - i].key == st.key) {
            if (i < ply) return true;  // ağaç içinde
            if (++count == 2) return true;
        }
    }
    return false;
}

Key Position::compute_key() const {
    Key k = sideToMove == BLACK ? Zobrist::side : 0;
    for (Bitboard b = pieces(); b;) {
        Square s = pop_lsb(b);
        k ^= Zobrist::psq[board[s]][s];
    }
    if (ep_square() != SQ_NONE) k ^= Zobrist::enpassant[file_of(ep_square())];
    return k ^ Zobrist::castling[castling_rights()];
}

Key Position::compute_pawn_key() const {
    Key k = 0;
    for (Bitboard b = pieces(PAWN); b;) {
        Square s = pop_lsb(b);
        k ^= Zobrist::psq[board[s]][s];
    }
    return k;
}

bool Position::pos_is_ok(std::string* why) const {
    auto fail = [&](const char* msg) { if (why) *why = msg; return false; };
    Bitboard all = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece pc = board[s];
        if (pc == NO_PIECE) continue;
        all |= square_bb(s);
        if (!(pieces(color_of(pc), type_of(pc)) & square_bb(s))) return fail("board/bitboard uyumsuz");
    }
    if (all != pieces()) return fail("dolu kareler uyumsuz");
    if (pieces(WHITE) & pieces(BLACK)) return fail("renkler çakışıyor");
    if ((pieces(WHITE) | pieces(BLACK)) != pieces()) return fail("renk birleşimi yanlış");
    for (PieceType a = PAWN; a <= KING; a = PieceType(a + 1))
        for (PieceType b = PieceType(a + 1); b <= KING; b = PieceType(b + 1))
            if (pieces(a) & pieces(b)) return fail("taş tipleri çakışıyor");
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return fail("şah sayısı");
    if (key() != compute_key()) return fail("zobrist anahtarı yanlış");
    if (pawn_key() != compute_pawn_key()) return fail("piyon anahtarı yanlış");
    if (checkers() != (attackers_to(king_square(sideToMove)) & pieces(~sideToMove))) return fail("checkers yanlış");
    if (attackers_to(king_square(~sideToMove)) & pieces(sideToMove)) return fail("rakip şah tehdit altında");
    return true;
}

}  // namespace mai
