// MaiEngine v3 — giriş noktası
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bitboard.h"
#include "misc.h"
#include "movegen.h"
#include "perft.h"
#include "position.h"

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
                 "  maiengine bench                      perft hız ölçümü\n";
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

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
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
    if (cmd == "bench") {
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
