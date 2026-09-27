// MaiEngine v3 — arama, iş parçacıkları, zaman yönetimi
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Yapı ve parametreler Stockfish 15.1'deki aramayı örnek alır (GPLv3,
// https://github.com/official-stockfish/Stockfish). Kod bizim; formüller ve
// sabitler oradan uyarlandı. Her budama/uzatma maçla ölçülecek.
#include "search.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

#include "evaluate.h"
#include "movegen.h"
#include "movepick.h"
#include "tt.h"

namespace mai {

ThreadPool Threads;
TimeManagement Time;
Search::Options Search::options;

namespace {

enum NodeType { NonPV, PV, Root };

int Reductions[MAX_MOVES];

Value futility_margin(Depth d, bool improving) { return Value(165 * (d - improving)); }

Depth reduction(bool improving, Depth d, int mn, Value delta, Value rootDelta) {
    int r = Reductions[std::min(d, MAX_MOVES - 1)] * Reductions[std::min(mn, MAX_MOVES - 1)];
    return (r + 1449 - int(delta) * 1032 / int(rootDelta)) / 1024 + (!improving && r > 941);
}

constexpr int futility_move_count(bool improving, Depth depth) {
    return improving ? (3 + depth * depth) : (3 + depth * depth) / 2;
}

int stat_bonus(Depth d) { return std::min((12 * d + 282) * d - 349, 1594); }

// Tekrar beraberliğinde küçük rastgelelik: arama beraberlik çizgisine kör olmasın
Value value_draw(const Thread* th) { return VALUE_DRAW - 1 + Value(th->nodes.load(std::memory_order_relaxed) & 0x2); }

// Mat skorları TT'de "bu düğümden mat" olarak saklanır
Value value_to_tt(Value v, int ply) {
    return v >= VALUE_TB_WIN_IN_MAX_PLY ? v + ply : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
}

Value value_from_tt(Value v, int ply, int r50c) {
    if (v == VALUE_NONE) return VALUE_NONE;
    if (v >= VALUE_TB_WIN_IN_MAX_PLY) {
        // 50 hamle kuralı yüzünden gerçekleşemeyecek mat skorlarını güvenilmez say
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 99 - r50c) return VALUE_MATE_IN_MAX_PLY - 1;
        return v - ply;
    }
    if (v <= VALUE_TB_LOSS_IN_MAX_PLY) {
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 99 - r50c) return VALUE_MATED_IN_MAX_PLY + 1;
        return v + ply;
    }
    return v;
}

void update_pv(Move* pv, Move move, const Move* childPv) {
    for (*pv++ = move; childPv && *childPv;) *pv++ = *childPv++;
    *pv = Move::none();
}

void update_continuation_histories(Search::Stack* ss, Piece pc, Square to, int bonus) {
    for (int i : {1, 2, 4, 6}) {
        // Şahtayken sadece yakın geçmiş anlamlı
        if (ss->inCheck && i > 2) break;
        if ((ss - i)->currentMove.is_ok()) (*(ss - i)->continuationHistory)(pc, to) << bonus;
    }
}

std::string pv_string(const std::vector<Move>& pv) {
    std::string s;
    for (Move m : pv) s += " " + move_to_uci(m);
    return s;
}

}  // namespace

std::string uci_score(Value v) {
    if (std::abs(v) < VALUE_MATE_IN_MAX_PLY) return "cp " + std::to_string(Eval::to_cp(v));
    return "mate " + std::to_string((v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2);
}

void Search::init() {
    for (int i = 1; i < MAX_MOVES; ++i)
        Reductions[i] = int((20.26 + std::log(double(Threads.size())) / 2) * std::log(double(i)));
}

void Search::clear() {
    Threads.main()->wait_for_search_finished();
    TT.clear();
    Threads.clear();
}

// ---------------------------------------------------------------------------
//  Zaman yönetimi
// ---------------------------------------------------------------------------
// 1+0.01 gibi çok kısa sürelerde bile ASLA süreden kaybetmemek esas:
// maximum = bu hamlede aşılmaması gereken sınır, hard = ne olursa olsun durma sınırı.
void TimeManagement::init(const Search::LimitsType& limits, Color us, int ply) {
    startTime = limits.startTime;
    if (!limits.use_time()) {
        optimumTime = maximumTime = hardTime = limits.movetime ? limits.movetime : 0;
        return;
    }
    const TimePoint overhead = Search::options.moveOverhead;
    const TimePoint myTime = limits.time[us], myInc = limits.inc[us];
    const int mtg = limits.movestogo ? std::min(limits.movestogo, 50) : 50;

    // Kalan süre tahmini (gelecekteki artışlar dahil, her hamlede overhead düş)
    TimePoint timeLeft = std::max<TimePoint>(1, myTime + myInc * (mtg - 1) - overhead * (2 + mtg));

    double optScale, maxScale;
    if (!limits.movestogo) {
        optScale = std::min(0.0084 + std::pow(ply + 3.0, 0.5) * 0.0042, 0.2 * myTime / double(timeLeft));
        maxScale = std::min(7.0, 4.0 + ply / 12.0);
    } else {
        optScale = std::min((0.88 + ply / 116.4) / mtg, 0.88 * myTime / double(timeLeft));
        maxScale = std::min(6.3, 1.5 + 0.11 * mtg);
    }
    optimumTime = TimePoint(optScale * timeLeft);
    maximumTime = TimePoint(std::min(0.8 * myTime - overhead, maxScale * optimumTime)) - 10;
    // Mutlak sınır: saatin sıfırlanmasına asla yaklaşma
    hardTime = std::max<TimePoint>(1, std::min<TimePoint>(myTime - overhead - 5, myTime / 2 + myInc));
    maximumTime = std::clamp<TimePoint>(maximumTime, 1, hardTime);
    optimumTime = std::clamp<TimePoint>(optimumTime, 1, maximumTime);
}

// ---------------------------------------------------------------------------
//  İş parçacıkları
// ---------------------------------------------------------------------------
Thread::Thread(size_t index) : idx(index), stdThread(&Thread::idle_loop, this) {
    wait_for_search_finished();
}

Thread::~Thread() {
    {
        std::lock_guard<std::mutex> lk(mutex);
        exit = true;
        searching = true;
    }
    cv.notify_one();
    stdThread.join();
}

void Thread::clear() {
    counterMoves.clear();
    mainHistory.fill(0);
    captureHistory.fill(0);
    for (bool inCheck : {false, true})
        for (int c : {0, 1}) {
            continuationHistory[inCheck][c].fill(-71);
            continuationHistory[inCheck][c](NO_PIECE, SQ_A1).fill(0);  // yığın başındaki nöbetçi
        }
    evaluator.clear();
    bestPreviousScore = bestPreviousAverageScore = VALUE_INFINITE;
    previousTimeReduction = 1.0;
}

void Thread::start_searching() {
    {
        std::lock_guard<std::mutex> lk(mutex);
        searching = true;
    }
    cv.notify_one();
}

void Thread::wait_for_search_finished() {
    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return !searching; });
}

void Thread::idle_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(mutex);
        searching = false;
        cv.notify_one();
        cv.wait(lk, [&] { return searching; });
        if (exit) return;
        lk.unlock();
        search_root();
    }
}

void ThreadPool::set(size_t n) {
    if (!threads.empty()) main()->wait_for_search_finished();
    threads.clear();
    for (size_t i = 0; i < n; ++i) threads.push_back(std::make_unique<Thread>(i));
    clear();
    Search::init();
}

void ThreadPool::clear() {
    for (auto& t : threads) t->clear();
}

uint64_t ThreadPool::nodes_searched() const {
    uint64_t n = 0;
    for (auto& t : threads) n += t->nodes.load(std::memory_order_relaxed);
    return n;
}

int ThreadPool::take_best_move_changes() {
    int n = 0;
    for (auto& t : threads) n += t->bestMoveChanges.exchange(0);
    return n;
}

void ThreadPool::wait_for_search_finished() {
    for (auto& t : threads) t->wait_for_search_finished();
}

void ThreadPool::start_helpers() {
    for (size_t i = 1; i < threads.size(); ++i) threads[i]->start_searching();
}

void ThreadPool::wait_helpers() {
    for (size_t i = 1; i < threads.size(); ++i) threads[i]->wait_for_search_finished();
}

void ThreadPool::start_thinking(const Position& pos, const Search::LimitsType& lim) {
    main()->wait_for_search_finished();
    stop = false;
    increaseDepth = true;
    limits = lim;

    std::vector<Search::RootMove> rootMoves;
    for (const auto& em : MoveList<LEGAL>(pos))
        if (lim.searchmoves.empty()
            || std::count(lim.searchmoves.begin(), lim.searchmoves.end(), em.move))
            rootMoves.emplace_back(em.move);

    for (auto& t : threads) {
        t->rootPos = pos;
        t->rootMoves = rootMoves;
        t->nodes = 0;
        t->bestMoveChanges = 0;
        t->rootDepth = t->completedDepth = 0;
        t->nmpMinPly = 0;
    }
    main()->start_searching();
}

// Oylama: daha derin ve daha iyi skorlu iş parçacığının hamlesi kazanır
Thread* ThreadPool::get_best_thread() const {
    Thread* best = main();
    if (threads.size() == 1) return best;
    Value minScore = VALUE_NONE;
    for (auto& t : threads) minScore = std::min(minScore, t->rootMoves[0].score);
    std::vector<std::pair<Move, int64_t>> votes;
    auto vote_of = [&](Move m) -> int64_t& {
        for (auto& v : votes)
            if (v.first == m) return v.second;
        votes.emplace_back(m, 0);
        return votes.back().second;
    };
    for (auto& t : threads) vote_of(t->rootMoves[0].pv[0]) += int64_t(t->rootMoves[0].score - minScore + 14) * t->completedDepth;

    for (auto& t : threads) {
        Thread* th = t.get();
        const Value bestScore = best->rootMoves[0].score, thScore = th->rootMoves[0].score;
        if (std::abs(bestScore) >= VALUE_TB_WIN_IN_MAX_PLY) {
            if (thScore > bestScore) best = th;  // en kısa mat
        } else if (thScore >= VALUE_TB_WIN_IN_MAX_PLY
                   || (thScore > VALUE_TB_LOSS_IN_MAX_PLY
                       && vote_of(th->rootMoves[0].pv[0]) > vote_of(best->rootMoves[0].pv[0])))
            best = th;
    }
    return best;
}

// ---------------------------------------------------------------------------
//  Kök
// ---------------------------------------------------------------------------
void Thread::search_root() {
    if (!is_main()) {
        id_loop();
        return;
    }

    const auto& limits = Threads.limits;
    Time.init(limits, rootPos.side_to_move(), rootPos.game_ply());
    TT.new_search();

    if (rootMoves.empty()) {
        rootMoves.emplace_back(Move::none());
        if (!Threads.silent)
            std::cout << "info depth 0 score " << uci_score(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW) << std::endl;
    } else {
        Threads.start_helpers();
        id_loop();
    }

    // Sonsuz aramada "stop" gelene kadar bekle
    while (!Threads.stop && limits.infinite) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Threads.stop = true;
    Threads.wait_helpers();

    Thread* best = this;
    if (rootMoves[0].pv[0] && Threads.size() > 1 && !limits.depth) best = Threads.get_best_thread();

    bestPreviousScore = best->rootMoves[0].score;
    bestPreviousAverageScore = best->rootMoves[0].averageScore;

    if (!Threads.silent) {
        const auto& rm = best->rootMoves[0];
        if (best != this) {
            TimePoint el = std::max<TimePoint>(Time.elapsed(), 1);
            uint64_t n = Threads.nodes_searched();
            std::cout << "info depth " << best->completedDepth << " seldepth " << rm.selDepth << " score "
                      << uci_score(rm.uciScore) << " nodes " << n << " nps " << n * 1000 / el << " time " << el
                      << " pv" << pv_string(rm.pv) << std::endl;
        }
        std::cout << "bestmove " << move_to_uci(rm.pv[0]);
        if (rm.pv.size() > 1) std::cout << " ponder " << move_to_uci(rm.pv[1]);
        std::cout << std::endl;
    }
}

void Thread::check_time() {
    if (--callsCnt > 0) return;
    const auto& limits = Threads.limits;
    callsCnt = limits.nodes ? std::min(512, int(limits.nodes / 1024) + 1) : 512;

    const TimePoint el = Time.elapsed();
    if (limits.use_time()) {
        // En az 1. derinliği bitir (yoksa rastgele hamle oynanır), ama sert sınırı asla aşma
        if ((el > Time.maximum() && completedDepth >= 1) || el > Time.hard()) Threads.stop = true;
    } else if (limits.movetime && el >= limits.movetime)
        Threads.stop = true;
    if (limits.nodes && Threads.nodes_searched() >= limits.nodes) Threads.stop = true;
}

void Thread::id_loop() {
    using namespace Search;
    Stack stack[MAX_PLY + 10] = {};
    Stack* ss = stack + 7;
    Move pv[MAX_PLY + 1];

    for (int i = 7; i > 0; --i) {
        (ss - i)->continuationHistory = &continuationHistory[0][0](NO_PIECE, SQ_A1);
        (ss - i)->staticEval = VALUE_NONE;
    }
    for (int i = 0; i <= MAX_PLY + 2; ++i) (ss + i)->ply = i;
    ss->pv = pv;

    Value bestValue = -VALUE_INFINITE, alpha = -VALUE_INFINITE, beta = VALUE_INFINITE, delta = 0;
    Move lastBestMove = Move::none();
    Depth lastBestMoveDepth = 0;
    double timeReduction = 1, totBestMoveChanges = 0;
    int iterIdx = 0, searchAgainCounter = 0;

    if (is_main()) {
        const Value init = bestPreviousScore == VALUE_INFINITE ? VALUE_ZERO : bestPreviousScore;
        for (auto& v : iterValue) v = init;
        if (bestPreviousAverageScore == VALUE_INFINITE) bestPreviousAverageScore = VALUE_ZERO;
    }

    const auto& limits = Threads.limits;
    while (++rootDepth < MAX_PLY && !Threads.stop && !(limits.depth && is_main() && rootDepth > limits.depth)) {
        if (is_main()) totBestMoveChanges /= 2;
        for (auto& rm : rootMoves) rm.previousScore = rm.score;
        pvIdx = 0;
        pvLast = int(rootMoves.size());
        if (!Threads.increaseDepth) ++searchAgainCounter;
        selDepth = 0;

        // Aspiration penceresi: önceki skor civarında dar pencere
        if (rootDepth >= 4) {
            const Value prev = rootMoves[0].averageScore;
            delta = Value(10) + int(prev) * prev / 15620;
            alpha = std::max(prev - delta, -VALUE_INFINITE);
            beta = std::min(prev + delta, VALUE_INFINITE);
        }

        int failedHighCnt = 0;
        while (true) {
            const Depth adjustedDepth = std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
            rootDelta = std::max(1, beta - alpha);
            bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);
            std::stable_sort(rootMoves.begin(), rootMoves.end());
            if (Threads.stop) break;

            if (bestValue <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(bestValue - delta, -VALUE_INFINITE);
                failedHighCnt = 0;
            } else if (bestValue >= beta) {
                beta = std::min(bestValue + delta, VALUE_INFINITE);
                ++failedHighCnt;
            } else
                break;
            delta += delta / 4 + 2;
        }
        std::stable_sort(rootMoves.begin(), rootMoves.end());

        if (!Threads.stop) completedDepth = rootDepth;

        if (is_main() && !Threads.silent && (completedDepth == rootDepth)) {
            const auto& rm = rootMoves[0];
            const TimePoint el = std::max<TimePoint>(Time.elapsed(), 1);
            const uint64_t n = Threads.nodes_searched();
            std::cout << "info depth " << completedDepth << " seldepth " << rm.selDepth << " score "
                      << uci_score(rm.uciScore)
                      << (rm.scoreLowerbound ? " lowerbound" : rm.scoreUpperbound ? " upperbound" : "")
                      << " nodes " << n << " nps " << n * 1000 / el << " hashfull " << TT.hashfull() << " time "
                      << el << " pv" << pv_string(rm.pv) << std::endl;
        }

        if (rootMoves[0].pv[0] != lastBestMove) {
            lastBestMove = rootMoves[0].pv[0];
            lastBestMoveDepth = rootDepth;
        }

        if (!is_main()) continue;

        // İstenen mat bulunduysa dur
        if (limits.mate && bestValue >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - bestValue <= 2 * limits.mate)
            Threads.stop = true;

        // Zaman: skor düşüyorsa / en iyi hamle sık değişiyorsa daha çok düşün
        if (limits.use_time() && !Threads.stop) {
            double fallingEval = (69 + 12 * (bestPreviousAverageScore - bestValue) + 6 * (iterValue[iterIdx] - bestValue)) / 781.4;
            fallingEval = std::clamp(fallingEval, 0.5, 1.5);
            timeReduction = lastBestMoveDepth + 10 < completedDepth ? 1.63 : 0.73;
            const double reduction = (1.56 + previousTimeReduction) / (2.20 * timeReduction);
            // Tüm iş parçacıklarının en iyi hamle değişimlerini topla
            totBestMoveChanges += Threads.take_best_move_changes();
            const double instability = 1 + 1.7 * totBestMoveChanges / double(Threads.size());
            double totalTime = double(Time.optimum()) * fallingEval * reduction * instability;
            if (rootMoves.size() == 1) totalTime = std::min(500.0, totalTime);

            const TimePoint el = Time.elapsed();
            if (el > totalTime) Threads.stop = true;
            else Threads.increaseDepth = el <= totalTime * 0.53;
        }
        iterValue[iterIdx] = bestValue;
        iterIdx = (iterIdx + 1) & 3;
    }
    if (is_main()) previousTimeReduction = timeReduction;
}

// ---------------------------------------------------------------------------
//  Ana arama
// ---------------------------------------------------------------------------
template <int NT>
Value Thread::search(Position& pos, Search::Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
    using namespace Search;
    constexpr bool PvNode = NT != NonPV;
    constexpr bool rootNode = NT == Root;

    if (depth <= 0) return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    Move pv[MAX_PLY + 1], capturesSearched[32], quietsSearched[64];
    Value bestValue, value, ttValue, eval, maxValue = VALUE_INFINITE, probCutBeta;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth;
    bool givesCheck, improving, priorCapture, singularQuietLMR, capture, moveCountPruning, ttCapture;
    int moveCount = 0, captureCount = 0, quietCount = 0, improvement;
    Piece movedPiece;

    const Color us = pos.side_to_move();
    ss->inCheck = pos.checkers();
    priorCapture = pos.captured_piece();
    bestValue = -VALUE_INFINITE;

    if (is_main()) check_time();
    if (PvNode && selDepth < ss->ply + 1) selDepth = ss->ply + 1;

    if (!rootNode) {
        // Durma, beraberlik, en derin ply
        if (Threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? Eval::evaluate(pos, evaluator) : value_draw(this);

        // Mat mesafesi budaması: daha kısa bir mat zaten bulunduysa bu dal onu geçemez
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply + 1), beta);
        if (alpha >= beta) return alpha;
    } else
        rootDelta = std::max(1, beta - alpha);

    (ss + 1)->ttPv = false;
    (ss + 1)->excludedMove = bestMove = Move::none();
    (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move::none();
    (ss + 2)->cutoffCnt = 0;
    ss->doubleExtensions = (ss - 1)->doubleExtensions;
    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
    ss->statScore = 0;

    // --- TT ---
    excludedMove = ss->excludedMove;
    // Tekillik araması (bir hamle hariç) ayrı anahtar kullanır
    const Key posKey = excludedMove ? pos.key() ^ (Key(excludedMove.raw()) * 0x9E3779B97F4A7C15ULL) : pos.key();
    bool ttHit;
    TTEntry* tte = TT.probe(posKey, ttHit);
    ss->ttHit = ttHit;
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50()) : VALUE_NONE;
    ttMove = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? tte->move() : Move::none();
    ttCapture = ttMove && pos.capture_stage(ttMove);
    if (!excludedMove) ss->ttPv = PvNode || (ttHit && tte->is_pv());

    // TT kesmesi (PV olmayan düğümde, yeterince derin kayıt)
    if (!PvNode && ttHit && tte->depth() > depth - (tte->bound() == BOUND_EXACT) && ttValue != VALUE_NONE
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER))) {
        if (ttMove && pos.legal_move(ttMove)) {
            if (ttValue >= beta) {
                if (!ttCapture) update_quiet_stats(pos, ss, ttMove, stat_bonus(depth));
                // Önceki ply'ın erken sessiz hamlesine ceza
                if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            } else if (!ttCapture) {
                mainHistory(us, ttMove) << -stat_bonus(depth);
                update_continuation_histories(ss, pos.moved_piece(ttMove), ttMove.to_sq(), -stat_bonus(depth));
            }
        }
        if (pos.rule50() < 90) return ttValue;
    }

    // --- Statik değerlendirme ---
    if (ss->inCheck) {
        ss->staticEval = eval = VALUE_NONE;
        improving = false;
        improvement = 0;
        goto moves_loop;
    } else if (excludedMove) {
        eval = ss->staticEval;  // tekillik aramasında aynı düğüm
    } else if (ttHit) {
        ss->staticEval = eval = tte->eval();
        if (eval == VALUE_NONE) ss->staticEval = eval = Eval::evaluate(pos, evaluator);
        // TT değeri daha iyi bir tahmin olabilir
        if (ttValue != VALUE_NONE && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER))) eval = ttValue;
    } else {
        ss->staticEval = eval = Eval::evaluate(pos, evaluator);
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, Move::none(), eval);
    }

    // Önceki sessiz hamle pozisyonu iyileştirdi/kötüleştirdi mi? history'ye yansıt
    if ((ss - 1)->currentMove.is_ok() && !(ss - 1)->inCheck && !priorCapture) {
        int bonus = std::clamp(-19 * int((ss - 1)->staticEval + ss->staticEval), -1914, 1914);
        mainHistory(~us, (ss - 1)->currentMove) << bonus;
    }

    improvement = (ss - 2)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 2)->staticEval
                : (ss - 4)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 4)->staticEval
                                                     : 168;
    improving = improvement > 0;

    // --- Razoring: çok kötüyse direkt qsearch ---
    if (eval < alpha - 369 - 254 * depth * depth) {
        value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
        if (value < alpha) return value;
    }

    // --- Reverse futility: çok iyiyse keser ---
    if (!ss->ttPv && depth < 8 && eval - futility_margin(depth, improving) - (ss - 1)->statScore / 303 >= beta
        && eval >= beta && eval < 28031)
        return eval;

    // --- Null move ---
    if (!PvNode && (ss - 1)->currentMove != Move::null() && (ss - 1)->statScore < 17139 && eval >= beta
        && eval >= ss->staticEval && ss->staticEval >= beta - 20 * depth - improvement / 13 + 233 && !excludedMove
        && pos.non_pawn_material(us) && (ss->ply >= nmpMinPly || us != nmpColor)) {
        const Depth R = std::min(int(eval - beta) / 168, 7) + depth / 3 + 4;

        ss->currentMove = Move::null();
        ss->continuationHistory = &continuationHistory[0][0](NO_PIECE, SQ_A1);
        pos.do_null_move();
        Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);
        pos.undo_null_move();

        if (nullValue >= beta) {
            if (nullValue >= VALUE_TB_WIN_IN_MAX_PLY) nullValue = beta;  // doğrulanmamış mat skoru döndürme
            if (nmpMinPly || (std::abs(beta) < VALUE_KNOWN_WIN && depth < 14)) return nullValue;

            // Derin düğümde zugzwang'a karşı doğrulama araması
            nmpMinPly = ss->ply + 3 * (depth - R) / 4;
            nmpColor = us;
            Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
            nmpMinPly = 0;
            if (v >= beta) return nullValue;
        }
    }

    probCutBeta = beta + 180 - 54 * improving;

    // --- ProbCut: iyi bir alış sığ aramada beta'yı rahatça geçiyorsa kes ---
    if (!PvNode && depth > 4 && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        && !(ttHit && tte->depth() >= depth - 3 && ttValue != VALUE_NONE && ttValue < probCutBeta)) {
        MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);
        while ((move = mp.next_move())) {
            if (move == excludedMove) continue;
            ss->currentMove = move;
            ss->continuationHistory = &continuationHistory[ss->inCheck][true](pos.moved_piece(move), move.to_sq());
            nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            pos.do_move(move);
            value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);
            if (value >= probCutBeta)
                value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, !cutNode);
            pos.undo_move(move);
            if (value >= probCutBeta) {
                tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER, depth - 3, move, ss->staticEval);
                return value;
            }
        }
    }

    // --- Internal iterative reduction: TT hamlesi yoksa sığ ara ---
    if (PvNode && !ttMove) depth -= 3;
    if (depth <= 0) return qsearch<PV>(pos, ss, alpha, beta);
    if (cutNode && depth >= 9 && !ttMove) depth -= 2;

moves_loop:
    // Şahtayken küçük ProbCut: TT'deki alış zaten çok iyi
    probCutBeta = beta + 481;
    if (ss->inCheck && !PvNode && depth >= 2 && ttCapture && (tte->bound() & BOUND_LOWER) && tte->depth() >= depth - 3
        && ttValue >= probCutBeta && std::abs(ttValue) <= VALUE_KNOWN_WIN && std::abs(beta) <= VALUE_KNOWN_WIN)
        return probCutBeta;

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory, (ss - 2)->continuationHistory, nullptr,
                                        (ss - 4)->continuationHistory, nullptr, (ss - 6)->continuationHistory};
    const Move countermove = prevSq != SQ_NONE ? counterMoves(pos.piece_on(prevSq), prevSq) : Move::none();

    MovePicker mp(pos, ttMove, depth, &mainHistory, &captureHistory, contHist, countermove, ss->killers);

    value = bestValue;
    moveCountPruning = singularQuietLMR = false;
    const bool likelyFailLow = PvNode && ttMove && (tte->bound() & BOUND_UPPER) && tte->depth() >= depth;

    while ((move = mp.next_move(moveCountPruning))) {
        if (move == excludedMove) continue;
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move)) continue;

        ss->moveCount = ++moveCount;
        if (PvNode) (ss + 1)->pv = nullptr;

        extension = 0;
        capture = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);
        newDepth = depth - 1;
        const Value delta = beta - alpha;

        // --- Sığ derinlikte budama ---
        if (!rootNode && pos.non_pawn_material(us) && bestValue > VALUE_TB_LOSS_IN_MAX_PLY) {
            moveCountPruning = moveCount >= futility_move_count(improving, depth);
            const int lmrDepth = std::max(newDepth - reduction(improving, depth, moveCount, delta, rootDelta), 0);

            if (capture || givesCheck) {
                const Piece captured = move.type_of() == EN_PASSANT ? make_piece(~us, PAWN) : pos.piece_on(move.to_sq());
                // Alışlarda futility
                if (!givesCheck && !PvNode && lmrDepth < 7 && !ss->inCheck
                    && ss->staticEval + 185 + 203 * lmrDepth + PieceValueEg[captured]
                               + captureHistory(movedPiece, move.to_sq(), type_of(captured)) / 6
                           < alpha)
                    continue;
                // SEE budaması (şah çeken hamleler dahil — SEE şah kuralını biliyor)
                if (!pos.see_ge(move, -220 * depth)) continue;
            } else {
                int history = (*contHist[0])(movedPiece, move.to_sq()) + (*contHist[1])(movedPiece, move.to_sq())
                            + (*contHist[3])(movedPiece, move.to_sq());
                // Continuation history budaması
                if (lmrDepth < 5 && history < -3875 * (depth - 1)) continue;
                history += 2 * mainHistory(us, move);
                // Sessiz hamlelerde futility
                if (!ss->inCheck && lmrDepth < 13 && ss->staticEval + 106 + 145 * lmrDepth + history / 52 <= alpha)
                    continue;
                // SEE budaması
                if (!pos.see_ge(move, -24 * lmrDepth * lmrDepth - 15 * lmrDepth)) continue;
            }
        }

        // --- Uzatmalar ---
        if (ss->ply < rootDepth * 2) {
            // Tekil uzatma: TT hamlesi diğer tüm hamlelerden belirgin iyiyse onu derin ara
            if (!rootNode && depth >= 4 - (completedDepth > 22) + 2 * (PvNode && tte->is_pv()) && move == ttMove
                && !excludedMove && std::abs(ttValue) < VALUE_KNOWN_WIN && (tte->bound() & BOUND_LOWER)
                && tte->depth() >= depth - 3) {
                const Value singularBeta = ttValue - (3 + (ss->ttPv && !PvNode)) * depth;
                const Depth singularDepth = (depth - 1) / 2;

                ss->excludedMove = move;
                value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
                ss->excludedMove = Move::none();

                if (value < singularBeta) {
                    extension = 1;
                    singularQuietLMR = !ttCapture;
                    if (!PvNode && value < singularBeta - 25 && ss->doubleExtensions <= 9) extension = 2;
                }
                // Çoklu kesme: TT hamlesi olmadan da beta geçiliyor
                else if (singularBeta >= beta)
                    return singularBeta;
                // Negatif uzatma
                else if (ttValue >= beta)
                    extension = -2;
                else if (ttValue <= alpha)
                    extension = -1;
            } else if (givesCheck && depth > 9 && std::abs(ss->staticEval) > 82)
                extension = 1;
            else if (PvNode && move == ttMove && move == ss->killers[0]
                     && (*contHist[0])(movedPiece, move.to_sq()) >= 5177)
                extension = 1;
        }

        newDepth += extension;
        ss->doubleExtensions = (ss - 1)->doubleExtensions + (extension == 2);

        ss->currentMove = move;
        ss->continuationHistory = &continuationHistory[ss->inCheck][capture](movedPiece, move.to_sq());

        nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        pos.do_move(move);

        // --- LMR ---
        if (depth >= 2 && moveCount > 1 + (PvNode && ss->ply <= 1)
            && (!ss->ttPv || !capture || (cutNode && (ss - 1)->moveCount > 1))) {
            Depth r = reduction(improving, depth, moveCount, delta, rootDelta);

            if (ss->ttPv && !likelyFailLow) r -= 2;
            if ((ss - 1)->moveCount > 7) r--;
            if (cutNode) r += 2;
            if (ttCapture) r++;
            if (PvNode) r -= 1 + 11 / (3 + depth);
            if (singularQuietLMR) r--;
            if ((ss + 1)->cutoffCnt > 3 && !PvNode) r++;

            ss->statScore = 2 * mainHistory(us, move) + (*contHist[0])(movedPiece, move.to_sq())
                          + (*contHist[1])(movedPiece, move.to_sq()) + (*contHist[3])(movedPiece, move.to_sq()) - 4433;
            r -= ss->statScore / (13628 + 4000 * (depth > 7 && depth < 19));

            const Depth d = std::clamp(newDepth - r, 1, newDepth + 1);
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

            // İndirgenmiş arama alpha'yı geçtiyse tam derinlikte tekrar
            if (value > alpha && d < newDepth) {
                const bool doDeeperSearch = value > (alpha + 64 + 11 * (newDepth - d));
                newDepth += doDeeperSearch;
                value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                int bonus = value > alpha ? stat_bonus(newDepth) : -stat_bonus(newDepth);
                if (capture) bonus /= 6;
                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        } else if (!PvNode || moveCount > 1)
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

        // PV düğümünde ilk hamle ya da pencereye düşen hamle: tam pencere
        if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta)))) {
            (ss + 1)->pv = pv;
            (ss + 1)->pv[0] = Move::none();
            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        pos.undo_move(move);

        if (Threads.stop.load(std::memory_order_relaxed)) return VALUE_ZERO;

        if (rootNode) {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);
            rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;
            if (moveCount == 1 || value > alpha) {
                rm.score = rm.uciScore = value;
                rm.selDepth = selDepth;
                rm.scoreLowerbound = rm.scoreUpperbound = false;
                if (value >= beta) {
                    rm.scoreLowerbound = true;
                    rm.uciScore = beta;
                } else if (value <= alpha) {
                    rm.scoreUpperbound = true;
                    rm.uciScore = alpha;
                }
                rm.pv.resize(1);
                for (Move* m = (ss + 1)->pv; m && *m; ++m) rm.pv.push_back(*m);
                if (moveCount > 1 && !pvIdx) ++bestMoveChanges;
            } else
                rm.score = -VALUE_INFINITE;  // sıralamada sona
        }

        if (value > bestValue) {
            bestValue = value;
            if (value > alpha) {
                bestMove = move;
                if (PvNode && !rootNode) update_pv(ss->pv, move, (ss + 1)->pv);
                if (PvNode && value < beta) {
                    alpha = value;
                    // Kalan hamleleri biraz daha sığ ara
                    if (depth > 1 && depth < 6 && beta < VALUE_KNOWN_WIN && alpha > -VALUE_KNOWN_WIN) depth -= 1;
                } else {
                    ss->cutoffCnt++;
                    break;  // beta kesmesi
                }
            }
        }

        if (move != bestMove) {
            if (capture && captureCount < 32) capturesSearched[captureCount++] = move;
            else if (!capture && quietCount < 64) quietsSearched[quietCount++] = move;
        }
    }

    // Hiç legal hamle yoksa mat ya da pat
    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;
    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq, quietsSearched, quietCount, capturesSearched,
                         captureCount, depth);
    // Alpha geçilemediyse: bu düğüme yol açan hamle iyi bir cevaptı
    else if ((depth >= 5 || PvNode) && !priorCapture && prevSq != SQ_NONE)
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      stat_bonus(depth) * (1 + (PvNode || cutNode)));

    if (PvNode) bestValue = std::min(bestValue, maxValue);

    if (bestValue <= alpha) ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

    if (!excludedMove && !(rootNode && pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta    ? BOUND_LOWER
                  : PvNode && bestMove ? BOUND_EXACT
                                       : BOUND_UPPER,
                  depth, bestMove, ss->staticEval);

    return bestValue;
}

// ---------------------------------------------------------------------------
//  Quiescence
// ---------------------------------------------------------------------------
template <int NT>
Value Thread::qsearch(Position& pos, Search::Stack* ss, Value alpha, Value beta) {
    using namespace Search;
    constexpr bool PvNode = NT == PV;

    Move pv[MAX_PLY + 1];
    Move ttMove, move, bestMove;
    Value bestValue, value, ttValue, futilityValue, futilityBase;
    bool pvHit, givesCheck, capture;
    int moveCount = 0, quietCheckEvasions = 0;

    if (PvNode) {
        (ss + 1)->pv = pv;
        ss->pv[0] = Move::none();
    }

    bestMove = Move::none();
    ss->inCheck = pos.checkers();

    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->inCheck) ? Eval::evaluate(pos, evaluator) : VALUE_DRAW;

    const Depth ttDepth = DEPTH_QS;
    const Key posKey = pos.key();
    bool ttHit;
    TTEntry* tte = TT.probe(posKey, ttHit);
    ss->ttHit = ttHit;
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50()) : VALUE_NONE;
    ttMove = ttHit ? tte->move() : Move::none();
    pvHit = ttHit && tte->is_pv();

    if (!PvNode && ttHit && tte->depth() >= ttDepth && ttValue != VALUE_NONE
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    if (ss->inCheck) {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    } else {
        if (ttHit) {
            if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                ss->staticEval = bestValue = Eval::evaluate(pos, evaluator);
            if (ttValue != VALUE_NONE && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        } else
            // Null move sonrası: değerlendirme sadece işaret değiştirir
            ss->staticEval = bestValue =
                (ss - 1)->currentMove != Move::null() ? Eval::evaluate(pos, evaluator) : -(ss - 1)->staticEval;

        // Stand pat
        if (bestValue >= beta) {
            if (!ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER, DEPTH_NONE, Move::none(),
                          ss->staticEval);
            return bestValue;
        }
        if (PvNode && bestValue > alpha) alpha = bestValue;
        futilityBase = bestValue + 118;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory, (ss - 2)->continuationHistory};
    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;

    MovePicker mp(pos, ttMove, &mainHistory, &captureHistory, contHist);

    while ((move = mp.next_move())) {
        givesCheck = pos.gives_check(move);
        capture = pos.capture_stage(move);
        ++moveCount;

        // Futility ve hamle sayısı budaması
        if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY && !givesCheck && move.to_sq() != prevSq
            && futilityBase > -VALUE_KNOWN_WIN && move.type_of() != PROMOTION) {
            if (moveCount > 2) continue;
            const Piece captured = move.type_of() == EN_PASSANT ? make_piece(~pos.side_to_move(), PAWN) : pos.piece_on(move.to_sq());
            futilityValue = futilityBase + PieceValueEg[captured];
            if (futilityValue <= alpha) {
                bestValue = std::max(bestValue, futilityValue);
                continue;
            }
            if (futilityBase <= alpha && !pos.see_ge(move, 1)) {
                bestValue = std::max(bestValue, futilityBase);
                continue;
            }
        }

        // Kaybettiren alışları arama
        if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY && !pos.see_ge(move)) continue;

        ss->currentMove = move;
        ss->continuationHistory = &continuationHistory[ss->inCheck][capture](pos.moved_piece(move), move.to_sq());

        // Continuation history kötü olan sessiz kaçışları atla
        if (!capture && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
            && (*contHist[0])(pos.moved_piece(move), move.to_sq()) < 0
            && (*contHist[1])(pos.moved_piece(move), move.to_sq()) < 0)
            continue;

        // İkinci sessiz şah kaçışından sonra dur
        if (quietCheckEvasions > 1) break;
        quietCheckEvasions += !capture && ss->inCheck;

        nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        pos.do_move(move);
        value = -qsearch<NT>(pos, ss + 1, -beta, -alpha);
        pos.undo_move(move);

        if (value > bestValue) {
            bestValue = value;
            if (value > alpha) {
                bestMove = move;
                if (PvNode) update_pv(ss->pv, move, (ss + 1)->pv);
                if (PvNode && value < beta) alpha = value;
                else break;
            }
        }
    }

    // Şahta ve hiç hamle aranmadıysa mat (budama olmadıysa)
    if (ss->inCheck && bestValue == -VALUE_INFINITE) return mated_in(ss->ply);

    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit, bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, ttDepth,
              bestMove, ss->staticEval);
    return bestValue;
}

// ---------------------------------------------------------------------------
//  İstatistik güncellemeleri
// ---------------------------------------------------------------------------
void Thread::update_quiet_stats(const Position& pos, Search::Stack* ss, Move move, int bonus) {
    if (ss->killers[0] != move) {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }
    const Color us = pos.side_to_move();
    mainHistory(us, move) << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus);
    if ((ss - 1)->currentMove.is_ok()) {
        const Square prevSq = (ss - 1)->currentMove.to_sq();
        counterMoves(pos.piece_on(prevSq), prevSq) = move;
    }
}

void Thread::update_all_stats(const Position& pos, Search::Stack* ss, Move bestMove, Value bestValue, Value beta,
                              Square prevSq, Move* quietsSearched, int quietCount, Move* capturesSearched,
                              int captureCount, Depth depth) {
    const Color us = pos.side_to_move();
    const Piece moved = pos.moved_piece(bestMove);
    const int bonus1 = stat_bonus(depth + 1);

    auto captured_type = [&](Move m) {
        return m.type_of() == EN_PASSANT ? PAWN : type_of(pos.piece_on(m.to_sq()));
    };

    if (!pos.capture_stage(bestMove)) {
        const int bonus2 = bestValue > beta + PawnValueMg ? bonus1 : stat_bonus(depth);
        update_quiet_stats(pos, ss, bestMove, bonus2);
        for (int i = 0; i < quietCount; ++i) {
            mainHistory(us, quietsSearched[i]) << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), quietsSearched[i].to_sq(), -bonus2);
        }
    } else
        captureHistory(moved, bestMove.to_sq(), captured_type(bestMove)) << bonus1;

    // Önceki ply'ın erken sessiz hamlesi burada çürütüldü: ceza
    if (prevSq != SQ_NONE
        && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit || (ss - 1)->currentMove == (ss - 1)->killers[0])
        && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -bonus1);

    for (int i = 0; i < captureCount; ++i)
        captureHistory(pos.moved_piece(capturesSearched[i]), capturesSearched[i].to_sq(),
                       captured_type(capturesSearched[i]))
            << -bonus1;
}

}  // namespace mai
