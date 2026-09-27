// MaiEngine v3 — UCI protokolü
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "uci.h"

#include <iostream>
#include <sstream>
#include <vector>

#include "evaluate.h"
#include "movegen.h"
#include "nnue.h"
#include "position.h"
#include "search.h"
#include "tt.h"

namespace mai::UCI {

namespace {

constexpr const char* EngineName = "MaiEngine 3.0-dev";

struct Settings {
    int hashMb = 64;
    int threads = 1;
    std::string evalFile = NNUE::DefaultNetName;
} settings;

bool try_load_net(const std::string& exeDir, const std::string& file) {
    std::string err;
    for (const std::string& p : {file, exeDir.empty() ? file : exeDir + "/" + file})
        if (NNUE::load(p, &err)) return true;
    std::cout << "info string HATA: NNUE ağı yüklenemedi: " << err << std::endl;
    return false;
}

void position_cmd(Position& pos, std::istringstream& is) {
    std::string token, fen;
    is >> token;
    if (token == "startpos") {
        fen = Position::StartFEN;
        is >> token;  // "moves" (varsa)
    } else if (token == "fen") {
        while (is >> token && token != "moves") fen += token + " ";
    } else
        return;
    if (!pos.set(fen)) {
        std::cout << "info string HATA: geçersiz FEN" << std::endl;
        pos.set(Position::StartFEN);
        return;
    }
    while (is >> token) {
        Move m = uci_to_move(pos, token);
        if (!m) {
            std::cout << "info string HATA: legal olmayan hamle " << token << std::endl;
            break;
        }
        pos.do_move(m);
    }
}

void go_cmd(const Position& pos, std::istringstream& is) {
    Search::LimitsType limits;
    limits.startTime = now();  // saati komut gelir gelmez başlat
    std::string token;
    while (is >> token) {
        if (token == "searchmoves")
            while (is >> token) limits.searchmoves.push_back(uci_to_move(pos, token));
        else if (token == "wtime") is >> limits.time[WHITE];
        else if (token == "btime") is >> limits.time[BLACK];
        else if (token == "winc") is >> limits.inc[WHITE];
        else if (token == "binc") is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "depth") is >> limits.depth;
        else if (token == "nodes") is >> limits.nodes;
        else if (token == "movetime") is >> limits.movetime;
        else if (token == "mate") is >> limits.mate;
        else if (token == "infinite") limits.infinite = true;
    }
    Threads.start_thinking(pos, limits);
}

void setoption_cmd(std::istringstream& is, const std::string& exeDir) {
    std::string token, name, value;
    is >> token;  // "name"
    while (is >> token && token != "value") name += (name.empty() ? "" : " ") + token;
    while (is >> token) value += (value.empty() ? "" : " ") + token;

    for (auto& c : name) c = char(std::tolower(c));
    if (name == "hash") {
        settings.hashMb = std::clamp(std::stoi(value), 1, 65536);
        Threads.main()->wait_for_search_finished();
        TT.resize(settings.hashMb);
    } else if (name == "threads") {
        settings.threads = std::clamp(std::stoi(value), 1, 512);
        Threads.set(settings.threads);
    } else if (name == "move overhead") {
        Search::options.moveOverhead = std::clamp(std::stoi(value), 0, 5000);
    } else if (name == "clear hash") {
        Search::clear();
    } else if (name == "evalfile") {
        settings.evalFile = value;
        Threads.main()->wait_for_search_finished();
        if (try_load_net(exeDir, value)) {
            Threads.clear();
            std::cout << "info string NNUE yüklendi: " << value << std::endl;
        }
    } else
        std::cout << "info string bilinmeyen seçenek: " << name << std::endl;
}

}  // namespace

void loop(const std::string& exeDir) {
    Position pos;
    std::string line, token;
    bool netOk = try_load_net(exeDir, settings.evalFile);
    TT.resize(settings.hashMb);
    Threads.set(settings.threads);

    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        token.clear();
        is >> std::skipws >> token;

        if (token == "quit") break;
        else if (token == "stop") Threads.stop = true;
        else if (token == "ponderhit") { /* ponder yok */ }
        else if (token == "uci") {
            std::cout << "id name " << EngineName << "\nid author Mai (Justmaii)\n"
                      << "option name Hash type spin default 64 min 1 max 65536\n"
                      << "option name Threads type spin default 1 min 1 max 512\n"
                      << "option name Move Overhead type spin default 10 min 0 max 5000\n"
                      << "option name Clear Hash type button\n"
                      << "option name EvalFile type string default " << NNUE::DefaultNetName << "\n"
                      << "uciok" << std::endl;
        } else if (token == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (token == "ucinewgame") {
            Search::clear();
        } else if (token == "setoption") {
            setoption_cmd(is, exeDir);
            netOk = NNUE::loaded();
        } else if (token == "position") {
            Threads.main()->wait_for_search_finished();
            position_cmd(pos, is);
        } else if (token == "go") {
            if (!netOk) {
                std::cout << "info string HATA: NNUE ağı yok, arama yapılamaz" << std::endl;
                MoveList<LEGAL> ml(pos);
                std::cout << "bestmove " << (ml.size() ? move_to_uci(ml.begin()->move) : "0000") << std::endl;
                continue;
            }
            go_cmd(pos, is);
        } else if (token == "d") {
            std::cout << pos.pretty() << std::endl;
        } else if (token == "eval") {
            if (netOk) {
                NNUE::Evaluator ev;
                int raw = ev.evaluate(pos);
                std::cout << "NNUE (ham, sıra gelen taraf): " << raw << "  (" << Eval::to_cp(raw) << " cp)\n"
                          << "Arama değerlendirmesi: " << Eval::to_cp(Eval::evaluate(pos, ev)) << " cp" << std::endl;
            }
        } else if (!token.empty())
            std::cout << "info string bilinmeyen komut: " << token << std::endl;
    }
    Threads.stop = true;
    Threads.main()->wait_for_search_finished();
}

void bench(int depth, int threads, int hashMb) {
    static const char* Fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
        "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
        "rq3rk1/ppp2ppp/1bnpb3/3N2B1/3NP3/7P/PPPQ1PP1/2KR3R w - - 7 14",
        "r1bq1r1k/1pp1n1pp/1p1p4/4p2Q/4Pp2/1BNP4/PPP2PPP/3R1RK1 w - - 2 14",
        "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
        "r1bbk1nr/pp3p1p/2n5/1N4p1/2Np1B2/8/PPP2PPP/2KR1B1R w kq - 0 13",
        "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
        "4r1k1/r1q2ppp/ppp2n2/4P3/5Rb1/1N1BQ3/PPP3PP/R5K1 w - - 1 17",
        "2rqkb1r/ppp2p2/2npb1p1/1N1Nn2p/2P1PP2/8/PP2B1PP/R1BQK2R b KQ - 0 11",
        "r1bq1b1r/ppp3pp/2n1k3/3np3/2B5/2N5/PPPP1PPP/R1BQK2R w KQ - 0 7",
        "3r1rk1/p5pp/bpp1pp2/8/q1PP1P2/b3P3/P2NQRPP/1R2B1K1 b - - 6 22",
        "r1q2rk1/2p1bppp/2Pp4/p6b/Q1PNp3/4B3/PP1R1PPP/2K4R w - - 2 18",
        "4k2r/1pb2ppp/1p2p3/1R1p4/3P4/2r1PN2/P4PPP/1R4K1 b - - 3 22",
        "3q2k1/pb3p1p/4pbp1/2r5/PpN2N2/1P2P2P/5PP1/Q2R2K1 b - - 4 26",
        "6k1/6p1/6Pp/ppp5/3pn2P/1P3K2/1PP2P2/3N4 b - - 0 1",
        "3b4/5kp1/1p1p1p1p/pP1PpP1P/P1P1P3/3KN3/8/8 w - - 0 1",
        "8/6pk/1p6/8/PP3p1p/5P2/4KP1q/3Q4 w - - 0 1",
        "7k/3p2pp/4q3/8/4Q3/5Kp1/P6b/8 w - - 0 1",
        "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
        "8/3k4/8/8/8/4B3/4KB2/2B5 w - - 0 1",
    };
    std::string err;
    if (!NNUE::loaded()) {
        std::cout << "NNUE ağı yok\n";
        return;
    }
    TT.resize(hashMb);
    Threads.set(threads);
    Threads.silent = true;
    Search::clear();

    uint64_t totalNodes = 0;
    TimePoint start = now();
    Position pos;
    for (const char* fen : Fens) {
        pos.set(fen);
        Search::LimitsType limits;
        limits.depth = depth;
        limits.startTime = now();
        Threads.start_thinking(pos, limits);
        Threads.main()->wait_for_search_finished();
        totalNodes += Threads.nodes_searched();
        std::cout << "  " << fen << "  -> " << move_to_uci(Threads.main()->rootMoves[0].pv[0]) << " "
                  << uci_score(Threads.main()->rootMoves[0].score) << "\n";
    }
    TimePoint ms = std::max<TimePoint>(now() - start, 1);
    Threads.silent = false;
    std::cout << "==========================\n"
              << "Toplam süre (ms) : " << ms << "\n"
              << "Düğüm            : " << totalNodes << "\n"
              << "Düğüm/saniye     : " << totalNodes * 1000 / ms << std::endl;
}

}  // namespace mai::UCI
