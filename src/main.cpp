// MaiEngine v3 — giriş noktası
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bitboard.h"
#include "misc.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "position.h"
#include "uci.h"

using namespace mai;

namespace {

void usage() {
    std::cout << "MaiEngine v3\n"
                 "Kullanım:\n"
                 "  maiengine perft <derinlik> [fen]     toplam düğüm sayısı\n"
                 "  maiengine divide <derinlik> [fen]    kök hamle başına düğüm\n"
                 "  maiengine suite [full]               bilinen perft sayıları (full: en derin)\n"
                 "  maiengine epd <dosya> [maxDerinlik]  EPD perft takımı (FEN ;D1 n ;D2 n ...)\n"
                 "  maiengine verify <derinlik> [fen]    ağaçta tutarlılık kontrolleri\n"
                 "  maiengine verifyepd <dosya> [d]      EPD'deki her pozisyonda verify\n"
                 "  maiengine perftbench                 perft hız ölçümü\n"
                 "  maiengine bench [d] [thr] [hashMB]   arama bench'i (düğüm sayısı = imza)\n"
                 "  maiengine nnuecheck <ref> [ağ]       NNUE'yi Stockfish referansıyla karşılaştır\n"
                 "  maiengine nnuebench [ağ]             NNUE hız ölçümü\n"
                 "  maiengine seecheck [epd]             SEE'yi kaba kuvvetle karşılaştır\n"
                 "  maiengine                            (argümansız) UCI modu\n";
}

std::string exeDir;

// Ağı sırayla dener: verilen yol, çalışma dizini, programın dizini
bool load_net(const std::string& given = "") {
    std::vector<std::string> tries;
    if (!given.empty()) tries.push_back(given);
    tries.push_back(NNUE::DefaultNetName);
    if (!exeDir.empty()) tries.push_back(exeDir + "/" + NNUE::DefaultNetName);
    std::string err;
    for (const auto& p : tries)
        if (NNUE::load(p, &err)) return true;
    std::cout << "info string NNUE ağı yüklenemedi (" << err << "). " << NNUE::DefaultNetName
              << " dosyasını programın yanına koy.\n";
    return false;
}

// Artımlı güncellemeyi sına: rastgele oyunlarda her düğümde sıfırdan hesapla karşılaştır.
// Ağaç içinde ileri-geri giderek tembel yığının atalarını ve Finny tablosunu da zorlar.
uint64_t nnue_incremental_check(Position& pos, NNUE::Evaluator& ev, int depth, PRNG& rng, uint64_t& nodes) {
    ++nodes;
    uint64_t errors = 0;
    int a = ev.evaluate(pos), b = NNUE::evaluate_scratch(pos);
    if (a != b) {
        if (errors++ < 5) std::cout << "HATA: artımlı " << a << " != sıfırdan " << b << "  " << pos.fen() << "\n";
    }
    if (depth == 0) return errors;
    MoveList<LEGAL> moves(pos);
    int n = 0;
    for (const auto& em : moves) {
        if (rng.rand64() % 4 && n > 0) continue;  // dalların bir kısmı
        ++n;
        pos.do_move(em.move);
        // Bazen bu düğümü değerlendirmeden derine in (tembel yolu sına)
        errors += nnue_incremental_check(pos, ev, depth - 1, rng, nodes);
        pos.undo_move(em.move);
    }
    return errors;
}

// Kaba kuvvet SEE referansı: sadece 'to' karesine legal (terfisiz) alışlarla minimax.
// Her taraf almayı bırakabilir. Legal hamle kullandığı için şah kuralı ve açmazlar kendiliğinden doğru.
int brute_see_rest(Position& pos, Square to) {
    int best = 0;
    for (const auto& em : MoveList<CAPTURES>(pos)) {
        Move m = em.move;
        if (m.to_sq() != to || m.type_of() != NORMAL) continue;
        int gain = PieceValueMg[pos.piece_on(to)];
        pos.do_move(m);
        gain -= brute_see_rest(pos, to);
        pos.undo_move(m);
        best = std::max(best, gain);
    }
    return best;
}

std::string join_fen(const std::vector<std::string>& args, size_t from) {
    if (args.size() <= from) return Position::StartFEN;
    std::string fen;
    for (size_t i = from; i < args.size(); ++i) fen += (i > from ? " " : "") + args[i];
    if (fen == "startpos") return Position::StartFEN;
    return fen;
}

}  // namespace

int main(int argc, char** argv) {
    Bitboards::init();
    Zobrist::init();
    {
        std::string self = argv[0];
        size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) exeDir = self.substr(0, slash);
    }

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "uci") {
        UCI::loop(exeDir);  // GUI'ler motoru argümansız başlatır
        return 0;
    }

    const std::string& cmd = args[0];
    Position pos;

    if ((cmd == "perft" || cmd == "divide" || cmd == "verify") && args.size() >= 2) {
        int depth = std::stoi(args[1]);
        if (!pos.set(join_fen(args, 2))) {
            std::cout << "FEN okunamadı\n";
            return 1;
        }
        if (cmd == "divide") {
            Perft::divide(pos, depth);
        } else if (cmd == "perft") {
            TimePoint start = now();
            uint64_t n = Perft::perft(pos, depth);
            TimePoint ms = std::max<TimePoint>(now() - start, 1);
            std::cout << "Nodes: " << n << "  " << ms << " ms  " << n / ms / 1000 << " Mnps\n";
        } else {
            uint64_t nodes = 0;
            uint64_t errors = Perft::verify(pos, depth, nodes);
            std::cout << nodes << " düğüm kontrol edildi, " << errors << " hata\n";
            return errors ? 1 : 0;
        }
        return 0;
    }
    if (cmd == "suite") return Perft::run_suite(args.size() > 1 && args[1] == "full") ? 0 : 1;
    if (cmd == "epd" && args.size() >= 2) return Perft::run_epd(args[1], args.size() > 2 ? std::stoi(args[2]) : 99) ? 0 : 1;
    if (cmd == "verifyepd" && args.size() >= 2) {
        // EPD'deki her pozisyonda verify (FEN, ';' işaretine kadar)
        int depth = args.size() > 2 ? std::stoi(args[2]) : 2;
        std::ifstream in(args[1]);
        std::string line;
        uint64_t nodes = 0, errors = 0, count = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!pos.set(line.substr(0, line.find(';')))) { ++errors; continue; }
            ++count;
            errors += Perft::verify(pos, depth, nodes);
        }
        std::cout << count << " pozisyon, " << nodes << " düğüm kontrol edildi, " << errors << " hata\n";
        return errors ? 1 : 0;
    }
    if (cmd == "nnuecheck") {
        // nnuecheck <referans dosyası> [ağ]: "FEN;değer" satırları, değer Stockfish'in ham NNUE çıktısı
        if (!load_net(args.size() > 2 ? args[2] : "")) return 1;
        std::cout << "Ağ: " << NNUE::description() << "\n";
        int total = 0, ok = 0;
        if (args.size() > 1) {
            std::ifstream in(args[1]);
            std::string line;
            NNUE::Evaluator ev;
            while (std::getline(in, line)) {
                size_t semi = line.rfind(';');
                if (semi == std::string::npos) continue;
                if (!pos.set(line.substr(0, semi))) continue;
                int expected = std::stoi(line.substr(semi + 1));
                int got = NNUE::evaluate_scratch(pos), got2 = ev.evaluate(pos);
                ++total;
                if (got == expected && got2 == expected) ++ok;
                else if (total - ok <= 5)
                    std::cout << "FARK: beklenen " << expected << ", sıfırdan " << got << ", artımlı " << got2 << "  "
                              << line.substr(0, semi) << "\n";
            }
            std::cout << "Stockfish referansı: " << ok << "/" << total << " birebir\n";
        }
        // Artımlı = sıfırdan, ağaç içinde
        PRNG rng(12345);
        NNUE::Evaluator ev;
        uint64_t nodes = 0, errors = 0;
        for (const char* fen : {Position::StartFEN,
                                "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                                "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                                "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                                "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"}) {
            pos.set(fen);
            errors += nnue_incremental_check(pos, ev, 5, rng, nodes);
        }
        std::cout << "Artımlı kontrol: " << nodes << " düğüm, " << errors << " hata (tazeleme " << ev.refreshes
                  << ", güncelleme " << ev.updates << ")\n";
        return (ok == total && !errors) ? 0 : 1;
    }
    if (cmd == "seecheck") {
        // EPD'deki pozisyonlarda her normal alış için see_ge'yi kaba kuvvet referansla karşılaştır
        std::ifstream in(args.size() > 1 ? args[1] : "tools/perft_random.epd");
        std::string line;
        int caps = 0, mismatch = 0, kingCases = 0;
        const int thresholds[] = {-1000, -300, -100, -1, 0, 1, 100, 300, 1000};
        while (std::getline(in, line)) {
            if (!pos.set(line.substr(0, line.find(';')))) continue;
            for (const auto& em : MoveList<CAPTURES>(pos)) {
                Move m = em.move;
                if (m.type_of() != NORMAL || !pos.capture(m)) continue;
                // Terfi karesine alışlar SEE'nin kapsamı dışında (ikisi farklı sayar), atla
                if (rank_of(m.to_sq()) == RANK_1 || rank_of(m.to_sq()) == RANK_8) continue;
                ++caps;
                int val = PieceValueMg[pos.piece_on(m.to_sq())];
                pos.do_move(m);
                val -= brute_see_rest(pos, m.to_sq());
                pos.undo_move(m);
                if (type_of(pos.piece_on(m.from_sq())) == KING) ++kingCases;
                for (int t : thresholds)
                    if (pos.see_ge(m, t) != (val >= t)) {
                        if (++mismatch <= 8)
                            std::cout << "FARK: " << move_to_uci(m) << " değer " << val << " eşik " << t << "  "
                                      << pos.fen() << "\n";
                        break;
                    }
            }
        }
        std::cout << caps << " alış (" << kingCases << " şahla), " << mismatch << " uyuşmazlık\n";
        return 0;
    }
    if (cmd == "nnuebench") {
        // Arama benzeri yük: perft ağacında her düğümde değerlendir
        if (!load_net(args.size() > 1 ? args[1] : "")) return 1;
        NNUE::Evaluator ev;
        uint64_t evals = 0;
        int64_t sink = 0;
        std::function<void(int)> walk = [&](int d) {
            sink += ev.evaluate(pos);
            ++evals;
            if (!d) return;
            for (const auto& em : MoveList<LEGAL>(pos)) {
                pos.do_move(em.move);
                walk(d - 1);
                pos.undo_move(em.move);
            }
        };
        TimePoint start = now();
        pos.set("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        walk(3);
        pos.set(Position::StartFEN);
        walk(4);
        TimePoint ms = std::max<TimePoint>(now() - start, 1);
        std::cout << evals << " değerlendirme, " << ms << " ms, " << evals * 1000 / ms << " eval/s (tazeleme "
                  << ev.refreshes << ", güncelleme " << ev.updates << ", sink " << sink % 7 << ")\n";
        return 0;
    }
    if (cmd == "bench") {
        // bench [derinlik] [iş parçacığı] [hash MB]
        if (!load_net()) return 1;
        UCI::bench(args.size() > 1 ? std::stoi(args[1]) : 13, args.size() > 2 ? std::stoi(args[2]) : 1,
                   args.size() > 3 ? std::stoi(args[3]) : 16);
        return 0;
    }
    if (cmd == "perftbench") {
        pos.set(Position::StartFEN);
        TimePoint start = now();
        uint64_t n = Perft::perft(pos, 6);
        pos.set("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        n += Perft::perft(pos, 5);
        TimePoint ms = std::max<TimePoint>(now() - start, 1);
        std::cout << "bench: " << n << " düğüm, " << ms << " ms, " << n / ms / 1000 << " Mnps\n";
        return 0;
    }
    usage();
    return 1;
}
