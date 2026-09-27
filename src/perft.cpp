// MaiEngine v3 — perft (hamle üretimi doğrulama)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "perft.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "misc.h"
#include "movegen.h"

namespace mai::Perft {

uint64_t perft(Position& pos, int depth) {
    MoveList<LEGAL> moves(pos);
    if (depth <= 1) return depth == 1 ? moves.size() : 1;  // toplu sayım: son katmanı oynamaya gerek yok
    uint64_t nodes = 0;
    for (const auto& em : moves) {
        pos.do_move(em.move);
        nodes += perft(pos, depth - 1);
        pos.undo_move(em.move);
    }
    return nodes;
}

uint64_t divide(Position& pos, int depth) {
    uint64_t total = 0;
    TimePoint start = now();
    for (const auto& em : MoveList<LEGAL>(pos)) {
        uint64_t n = 1;
        if (depth > 1) {
            pos.do_move(em.move);
            n = perft(pos, depth - 1);
            pos.undo_move(em.move);
        }
        total += n;
        std::cout << move_to_uci(em.move) << ": " << n << "\n";
    }
    TimePoint ms = std::max<TimePoint>(now() - start, 1);
    std::cout << "\nNodes searched: " << total << "\n"
              << "Time: " << ms << " ms, " << total * 1000 / ms / 1000 << " knps\n";
    return total;
}

uint64_t verify(Position& pos, int depth, uint64_t& nodes) {
    uint64_t errors = 0;
    auto report = [&](const std::string& msg) {
        if (errors++ < 10) std::cout << "HATA: " << msg << "\n  fen: " << pos.fen() << "\n";
    };

    ++nodes;
    std::string why;
    if (!pos.pos_is_ok(&why)) report("pos_is_ok: " + why);

    MoveList<LEGAL> all(pos);
    MoveList<CAPTURES> caps(pos);
    MoveList<QUIETS> quiets(pos);
    if (caps.size() + quiets.size() != all.size())
        report("CAPTURES + QUIETS != LEGAL (" + std::to_string(caps.size()) + "+" + std::to_string(quiets.size())
               + " vs " + std::to_string(all.size()) + ")");
    for (const auto& em : caps)
        if (!all.contains(em.move)) report("CAPTURES fazladan hamle: " + move_to_uci(em.move));
    for (const auto& em : quiets)
        if (!all.contains(em.move)) report("QUIETS fazladan hamle: " + move_to_uci(em.move));
    for (const auto& em : all) {
        bool isCap = pos.capture(em.move);
        bool isQueenPromo = em.move.type_of() == PROMOTION && em.move.promotion_type() == QUEEN;
        bool inCaps = caps.contains(em.move);
        if (inCaps != (isCap || isQueenPromo)) report("hamle yanlış listede: " + move_to_uci(em.move));
    }

    // FEN gidiş-dönüş: aynı pozisyon ve aynı anahtar
    Position copy;
    copy.set(pos.fen());
    if (copy.fen() != pos.fen() || copy.key() != pos.key() || copy.pawn_key() != pos.pawn_key())
        report("FEN gidiş-dönüş anahtarı tutmuyor");

    if (depth <= 0) return errors;
    for (const auto& em : all) {
        std::string before = pos.fen();
        Key k = pos.key();
        pos.do_move(em.move);
        errors += verify(pos, depth - 1, nodes);
        pos.undo_move(em.move);
        if (pos.fen() != before || pos.key() != k) report("undo_move pozisyonu geri getirmedi: " + move_to_uci(em.move));
    }
    // Null move
    if (!pos.checkers()) {
        Key k = pos.key();
        pos.do_null_move();
        if (!pos.pos_is_ok(&why)) report("null move sonrası: " + why);
        pos.undo_null_move();
        if (pos.key() != k) report("null move geri alınamadı");
    }
    return errors;
}

namespace {

struct SuiteEntry {
    const char* name;
    const char* fen;
    std::vector<uint64_t> counts;  // derinlik 1, 2, 3, ...
    int quickDepth;
};

// Sayılar: chessprogramming.org "Perft Results"
const std::vector<SuiteEntry> Suite = {
    {"Başlangıç", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     {20, 400, 8902, 197281, 4865609, 119060324}, 5},
    {"Kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     {48, 2039, 97862, 4085603, 193690690}, 4},
    {"Pozisyon 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     {14, 191, 2812, 43238, 674624, 11030083, 178633661}, 6},
    {"Pozisyon 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     {6, 264, 9467, 422333, 15833292}, 5},
    {"Pozisyon 4 (ayna)", "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
     {6, 264, 9467, 422333, 15833292}, 5},
    {"Pozisyon 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     {44, 1486, 62379, 2103487, 89941194}, 4},
    {"Pozisyon 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     {46, 2079, 89890, 3894594, 164075551}, 4},
};

}  // namespace

bool run_suite(bool full) {
    bool ok = true;
    uint64_t totalNodes = 0;
    TimePoint start = now();
    Position pos;
    for (const auto& e : Suite) {
        if (!pos.set(e.fen)) {
            std::cout << e.name << ": FEN okunamadı\n";
            ok = false;
            continue;
        }
        int maxD = full ? int(e.counts.size()) : e.quickDepth;
        for (int d = 1; d <= maxD; ++d) {
            uint64_t n = perft(pos, d);
            totalNodes += n;
            bool pass = n == e.counts[d - 1];
            ok &= pass;
            if (!pass || d == maxD)
                std::cout << std::left << std::setw(20) << e.name << " d" << d << ": " << std::setw(11) << n
                          << (pass ? " OK" : " HATALI (beklenen " + std::to_string(e.counts[d - 1]) + ")") << "\n";
        }
    }
    TimePoint ms = std::max<TimePoint>(now() - start, 1);
    std::cout << (ok ? "TÜMÜ DOĞRU" : "HATA VAR") << " — " << totalNodes << " düğüm, " << ms << " ms, "
              << totalNodes / ms / 1000 << " Mnps\n";
    return ok;
}

bool run_epd(const std::string& file, int maxDepth) {
    std::ifstream in(file);
    if (!in) {
        std::cout << "Dosya açılamadı: " << file << "\n";
        return false;
    }
    std::string line;
    int lines = 0, failed = 0;
    uint64_t totalNodes = 0;
    TimePoint start = now();
    Position pos;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fen, tok;
        std::getline(ss, fen, ';');
        ++lines;
        if (!pos.set(fen)) {
            std::cout << "FEN okunamadı: " << fen << "\n";
            ++failed;
            continue;
        }
        while (std::getline(ss, tok, ';')) {
            std::istringstream ts(tok);
            std::string dtok;
            uint64_t expected;
            if (!(ts >> dtok >> expected) || dtok.size() < 2 || dtok[0] != 'D') continue;
            int d = std::stoi(dtok.substr(1));
            if (d > maxDepth) continue;
            uint64_t n = perft(pos, d);
            totalNodes += n;
            if (n != expected) {
                ++failed;
                std::cout << "HATALI d" << d << ": " << n << " (beklenen " << expected << ")  " << fen << "\n";
            }
        }
    }
    TimePoint ms = std::max<TimePoint>(now() - start, 1);
    std::cout << lines << " pozisyon, " << failed << " hata, " << totalNodes << " düğüm, " << ms << " ms\n";
    return failed == 0;
}

}  // namespace mai::Perft
