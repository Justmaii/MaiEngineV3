// MaiEngine v3 — arama, iş parçacıkları, zaman yönetimi
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "history.h"
#include "misc.h"
#include "nnue.h"
#include "position.h"

namespace mai {

namespace Search {

struct LimitsType {
    std::vector<Move> searchmoves;
    TimePoint time[COLOR_NB] = {}, inc[COLOR_NB] = {}, movetime = 0, startTime = 0;
    int movestogo = 0, depth = 0, mate = 0;
    uint64_t nodes = 0;
    bool infinite = false;
    bool use_time() const { return time[WHITE] || time[BLACK]; }
};

// Her ply için arama yığını
struct Stack {
    Move* pv;
    PieceToHistory* continuationHistory;
    int ply;
    Move currentMove;
    Move excludedMove;
    Move killers[2];
    Value staticEval;
    int statScore;
    int moveCount;
    bool inCheck;
    bool ttPv;
    bool ttHit;
    int doubleExtensions;
    int cutoffCnt;
};

struct RootMove {
    explicit RootMove(Move m) : pv(1, m) {}
    bool operator==(Move m) const { return pv[0] == m; }
    // Büyükten küçüğe sırala (eşitse önceki iterasyonun skoru)
    bool operator<(const RootMove& m) const {
        return m.score != score ? m.score < score : m.previousScore < previousScore;
    }
    Value score = -VALUE_INFINITE;
    Value previousScore = -VALUE_INFINITE;
    Value averageScore = -VALUE_INFINITE;
    Value uciScore = -VALUE_INFINITE;
    bool scoreLowerbound = false, scoreUpperbound = false;
    int selDepth = 0;
    std::vector<Move> pv;
};

struct Options {
    int moveOverhead = 10;  // ms, GUI / ağ gecikmesi için ayrılan pay
};
extern Options options;

void init();   // LMR tablosu (iş parçacığı sayısına bağlı)
void clear();  // yeni oyun: TT + history'ler

}  // namespace Search

// Zaman yönetimi
class TimeManagement {
public:
    void init(const Search::LimitsType& limits, Color us, int ply);
    TimePoint optimum() const { return optimumTime; }
    TimePoint maximum() const { return maximumTime; }
    TimePoint hard() const { return hardTime; }
    TimePoint elapsed() const { return now() - startTime; }

private:
    TimePoint startTime = 0, optimumTime = 0, maximumTime = 0, hardTime = 0;
};
extern TimeManagement Time;

class Thread {
public:
    explicit Thread(size_t index);
    virtual ~Thread();
    void start_searching();
    void wait_for_search_finished();
    void clear();
    bool is_main() const { return idx == 0; }

    // Arama durumu
    size_t idx;
    Position rootPos;
    std::vector<Search::RootMove> rootMoves;
    NNUE::Evaluator evaluator;
    Depth rootDepth = 0, completedDepth = 0;
    int selDepth = 0, pvIdx = 0, pvLast = 0;
    std::atomic<uint64_t> nodes{0};
    std::atomic<int> bestMoveChanges{0};
    Value rootDelta = 1;
    int nmpMinPly = 0;
    Color nmpColor = WHITE;

    ButterflyHistory mainHistory;
    CaptureHistory captureHistory;
    ContinuationHistory continuationHistory[2][2];  // [şahta mı][alış mı]
    CounterMoveHistory counterMoves;

    // Sadece ana iş parçacığı (zaman yönetimi)
    Value bestPreviousScore = VALUE_INFINITE, bestPreviousAverageScore = VALUE_INFINITE;
    Value iterValue[4] = {};
    double previousTimeReduction = 1.0;
    int callsCnt = 0;

private:
    void idle_loop();
    void search_root();  // ana: arama + sonuç; yardımcı: sadece id_loop
    void id_loop();
    void check_time();

    template <int NT>
    Value search(Position& pos, Search::Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);
    template <int NT>
    Value qsearch(Position& pos, Search::Stack* ss, Value alpha, Value beta);

    void update_all_stats(const Position& pos, Search::Stack* ss, Move bestMove, Value bestValue, Value beta,
                          Square prevSq, Move* quietsSearched, int quietCount, Move* capturesSearched,
                          int captureCount, Depth depth);
    void update_quiet_stats(const Position& pos, Search::Stack* ss, Move move, int bonus);

    std::mutex mutex;
    std::condition_variable cv;
    bool exit = false, searching = true;
    std::thread stdThread;
};

class ThreadPool {
public:
    void set(size_t n);
    void clear();
    void start_thinking(const Position& pos, const Search::LimitsType& limits);
    void wait_for_search_finished();
    Thread* main() const { return threads.front().get(); }
    Thread* get_best_thread() const;
    size_t size() const { return threads.size(); }
    uint64_t nodes_searched() const;
    int take_best_move_changes();
    void start_helpers();
    void wait_helpers();

    std::atomic<bool> stop{false}, increaseDepth{true};
    Search::LimitsType limits;
    bool silent = false;  // bench / iç maç: info satırı basma

private:
    std::vector<std::unique_ptr<Thread>> threads;
};

extern ThreadPool Threads;

std::string uci_score(Value v);

}  // namespace mai
