// MaiEngine v3 — arama, iş parçacıkları, zaman yönetimi
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
//
// Arama, Stockfish 19'daki aramanın uyarlamasıdır (GPLv3,
// https://github.com/official-stockfish/Stockfish), üstüne geliştirme sürümü
// 49ea5ded'e kadarki arama değişiklikleri: budama, indirgeme ve uzatma
// kuralları, history ve correction history tabloları, aspiration penceresi,
// iyimserlik ve zaman yönetimi oradan. Tahta, hamle üretimi, NNUE çıkarımı,
// iş parçacıkları ve UCI bizim. Tablebase yok, MultiPV yok.
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

inline int lmr_divisor(int depth) {
    const int d = std::min(depth, 16);
    return 3000 + 7 * (d - 8) * (d - 8);
}

constexpr int SEARCHEDLIST_CAPACITY = 32;

template <typename T1, typename T2>
constexpr T2 interpolate(T1 x, T1 x0, T1 x1, T2 y0, T2 y1) {
    return T2(y0 + (y1 - y0) * (x - x0) / (x1 - x0));
}

// Correction history düzeltmesini ham değerlendirmeye ekle (mat/tablo aralığına girmeden)
Value to_corrected_static_eval(Value v, int cv) {
    return std::clamp(v + cv / 131072, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// Tekrar beraberliğinde küçük rastgelelik: arama beraberlik çizgisine kör olmasın
Value value_draw(uint64_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

// Mat / tablo skorları TT'de "bu düğümden" olarak saklanır
Value value_to_tt(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }

// 50 hamle kuralı yüzünden gerçekleşemeyecek mat skorlarını güvenilmez say
Value value_from_tt(Value v, int ply, int r50c) {
    if (!is_valid(v)) return VALUE_NONE;
    if (is_win(v)) {
        if (is_mate(v) && VALUE_MATE - v > 100 - r50c) return VALUE_TB_WIN_IN_MAX_PLY - 1;
        if (VALUE_TB - v > 100 - r50c) return VALUE_TB_WIN_IN_MAX_PLY - 1;
        return v - ply;
    }
    if (is_loss(v)) {
        if (is_mated(v) && VALUE_MATE + v > 100 - r50c) return VALUE_TB_LOSS_IN_MAX_PLY + 1;
        if (VALUE_TB + v > 100 - r50c) return VALUE_TB_LOSS_IN_MAX_PLY + 1;
        return v + ply;
    }
    return v;
}

void update_pv(Move* pv, Move move, const Move* childPv) {
    for (*pv++ = move; childPv && *childPv;) *pv++ = *childPv++;
    *pv = Move::none();
}

// Hamle çiftlerinin (önceki hamleler -> bu hamle) istatistikleri
void update_continuation_histories(Search::Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr int index[6] = {1, 2, 3, 4, 5, 6};
    static constexpr int weight[6] = {520, 390, 145, 251, 66, 209};
    // Tutarlı olumlu geçmiş için çarpanlar
    static constexpr int CMHCMultipliers[] = {94, 103, 110, 106, 119, 126, 121};
    int positiveCount = 0;

    for (int k = 0; k < 6; ++k) {
        const int i = index[k];
        // Şahtayken sadece yakın geçmiş anlamlı
        if (ss->inCheck && i > 2) break;
        if ((ss - i)->currentMove.is_ok()) {
            auto& entry = (*(ss - i)->continuationHistory)(pc, to);
            if (entry > 0) positiveCount++;
            entry << bonus * weight[k] * CMHCMultipliers[positiveCount] / 65536 + 73 * (i < 2);
        }
    }
}

// Aynı taşı ileri geri oynatan kısır döngüleri tespit et (arama patlamasın)
bool is_shuffling(Move move, const Search::Stack* ss, const Position& pos) {
    if (pos.capture_stage(move) || pos.rule50() < 10) return false;
    if (pos.state().pliesFromNull < 6 || ss->ply < 20) return false;
    return move.from_sq() == (ss - 2)->currentMove.to_sq()
        && (ss - 2)->currentMove.from_sq() == (ss - 4)->currentMove.to_sq();
}

std::string pv_string(const std::vector<Move>& pv) {
    std::string s;
    for (Move m : pv) s += " " + move_to_uci(m);
    return s;
}

}  // namespace

std::string uci_score(Value v, const Position& pos) {
    if (std::abs(v) < VALUE_MATE_IN_MAX_PLY) return "cp " + std::to_string(Eval::to_cp(v, pos));
    return "mate " + std::to_string((v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2);
}

void Search::init() {}

void Search::clear() {
    Threads.main()->wait_for_search_finished();
    TT.clear();
    Threads.clear();
    Time.originalTimeAdjust = -1;
}

// ---------------------------------------------------------------------------
//  Zaman yönetimi (Stockfish 19)
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
    const TimePoint scaledTime = std::max<TimePoint>(1, myTime);

    int mtg = limits.movestogo ? std::min(limits.movestogo, 50) : 50;
    // Bir saniyeden az kaldıysa ufku yavaşça kısalt
    if (scaledTime < 1000 && !limits.movestogo) mtg = int(scaledTime * 0.05);

    // Kalan süre tahmini (gelecekteki artışlar dahil, her hamlede overhead düş)
    const TimePoint timeLeft = std::max<TimePoint>(1, myTime + myInc * (mtg - 1) - overhead * (2 + mtg));

    double optScale, maxScale;
    if (!limits.movestogo) {
        if (originalTimeAdjust < 0) originalTimeAdjust = 0.3272 * std::log10(double(timeLeft)) - 0.4141;
        const double logTimeInSec = std::log10(scaledTime / 1000.0);
        const double optConstant = std::min(0.0029869 + 0.00033554 * logTimeInSec, 0.004905);
        const double maxConstant = std::max(3.3744 + 3.0608 * logTimeInSec, 3.1441);
        optScale = std::min(0.012112 + std::pow(ply + 3.22713, 0.46866) * optConstant,
                            0.19404 * myTime / double(timeLeft))
                 * originalTimeAdjust;
        maxScale = std::min(6.873, maxConstant + ply / 12.352);
    } else {
        optScale = std::min((0.88 + ply / 116.4) / mtg, 0.88 * myTime / double(timeLeft));
        maxScale = 1.3 + 0.11 * mtg;
    }
    // Saatte gerideysek daha az düşün (devirli sürede son hamlede değil: hesap yanıltır)
    if (limits.movestogo != 1) {
        const double timeAdvantage = (myTime - limits.time[~us]) / (1.0 + myTime + limits.time[~us]);
        optScale *= 1 + 0.9 * std::min(timeAdvantage, 0.0);
    }
    optimumTime = TimePoint(std::max(1.0, optScale * timeLeft));
    maximumTime = TimePoint(std::max(double(optimumTime), std::min(0.8097 * myTime - overhead, maxScale * optimumTime)));

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

// Yeni oyun: history'leri Stockfish 19'un başlangıç değerleriyle doldur
void Thread::clear() {
    mainHistory.fill(-5);
    lowPlyHistory.fill(0);
    captureHistory.fill(-742);
    correctionHistory.fill(-5);
    pawnHistory.fill(-1338);
    for (bool inCheck : {false, true})
        for (int c : {0, 1}) continuationHistory[inCheck][c].fill(-586);
    ttMoveHistory = 0;
    continuationCorrectionHistory.fill(5);

    reductions[0] = 0;
    for (int i = 1; i < MAX_MOVES; ++i) reductions[i] = int(2872 / 128.0 * std::log(double(i)));

    evaluator.clear();
    bestPreviousScore = bestPreviousAverageScore = VALUE_INFINITE;
    previousTimeReduction = 0.85;
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

// Oylama: skoru iyi olan iş parçacıklarının hamlesi daha çok oy alır
Thread* ThreadPool::get_best_thread() const {
    Thread* best = main();
    Value minScore = VALUE_INFINITE;
    for (auto& t : threads) minScore = std::min(minScore, t->rootMoves[0].score);

    std::vector<std::pair<Move, int64_t>> votes;
    auto vote_of = [&](Move m) -> int64_t& {
        for (auto& v : votes)
            if (v.first == m) return v.second;
        votes.emplace_back(m, 0);
        return votes.back().second;
    };
    for (auto& t : threads) vote_of(t->rootMoves[0].pv[0]) += t->rootMoves[0].score - minScore + 14;

    for (auto& t : threads) {
        const auto& bestMove = best->rootMoves[0];
        const auto& newMove = t->rootMoves[0];
        const int64_t bestVote = vote_of(bestMove.pv[0]), newVote = vote_of(newMove.pv[0]);
        // Yarıda kalan (d1) aramalar kesin olmayan mat skorları verebilir
        const bool bestDecisive = bestMove.score != -VALUE_INFINITE && is_decisive(bestMove.score) && !bestMove.is_inexact();
        const bool newDecisive = newMove.score != -VALUE_INFINITE && is_decisive(newMove.score) && !newMove.is_inexact();

        if (bestDecisive) {
            // En kısa matı seç
            if (newDecisive && std::abs(newMove.score) > std::abs(bestMove.score)) best = t.get();
        } else if (newDecisive
                   || (!is_loss(newMove.score)
                       && (newVote > bestVote || (newVote == bestVote && newMove.pv.size() > bestMove.pv.size()))))
            best = t.get();
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
            std::cout << "info depth 0 score " << uci_score(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW, rootPos) << std::endl;
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
                      << uci_score(rm.uciScore, rootPos) << " nodes " << n << " nps " << n * 1000 / el << " time " << el
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
        (ss - i)->continuationHistory = &continuationHistory[0][0](NO_PIECE, SQ_A1);  // nöbetçi
        (ss - i)->continuationCorrectionHistory = &continuationCorrectionHistory(NO_PIECE, SQ_A1);
        (ss - i)->staticEval = VALUE_NONE;
    }
    for (int i = 0; i <= MAX_PLY + 2; ++i) (ss + i)->ply = i;
    ss->pv = pv;

    std::vector<Move> lastBestMovePV;
    Depth lastBestMoveDepth = 0;
    Value lastBestMoveScore = -VALUE_INFINITE;
    Value bestValue = -VALUE_INFINITE, alpha, beta;
    const Color us = rootPos.side_to_move();
    double timeReduction = 1, totBestMoveChanges = 0;
    int delta, iterIdx = 0, searchAgainCounter = 0, failHighRecovery = 0;

    if (is_main())
        for (auto& v : iterValue) v = bestPreviousScore == VALUE_INFINITE ? VALUE_ZERO : bestPreviousScore;

    lowPlyHistory.fill(102);
    for (auto& side : mainHistory.t)
        for (auto& e : side) e = int(e) * 729 / 1024;

    const auto& limits = Threads.limits;
    while (rootDepth + 1 < MAX_PLY && !Threads.stop && !(limits.depth && is_main() && rootDepth >= limits.depth)) {
        ++rootDepth;
        if (is_main()) totBestMoveChanges /= 2;

        for (auto& rm : rootMoves) {
            rm.previousScore = rm.score;
            rm.previousPV = rm.pv;
        }
        pvIdx = 0;
        pvLast = int(rootMoves.size());
        if (!Threads.increaseDepth) ++searchAgainCounter;

        lastIterationIdxPV = rootMoves[0].previousPV;
        selDepth = 0;

        // Aspiration penceresi: kök hamlenin ortalama skoru civarında, skorlar
        // oynaksa (ortalama kare büyük) daha geniş
        delta = 5 + int(idx % 8) + std::abs(rootMoves[0].meanSquaredScore) / 10193;
        const Value avg = rootMoves[0].averageScore;
        alpha = std::max(avg - delta, -VALUE_INFINITE);
        beta = std::min(avg + delta, VALUE_INFINITE);

        // İyimserlik: kök skoru iyiyse değerlendirme biraz daha iyimser, kötüyse karamsar
        optimism[us] = 114 * avg / (std::abs(avg) + 85);
        optimism[~us] = -optimism[us];

        int failedHighCnt = 0;
        failHighRecovery = std::max(0, failHighRecovery - 2);
        while (true) {
            const Depth adjustedDepth =
                std::max(1, rootDepth - failedHighCnt - failHighRecovery - 3 * (searchAgainCounter + 1) / 4);
            rootDelta = beta - alpha;
            bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);
            std::stable_sort(rootMoves.begin(), rootMoves.end());
            if (Threads.stop) break;

            if (bestValue <= alpha) {
                beta = alpha;
                alpha = std::max(bestValue - delta, -VALUE_INFINITE);
                failedHighCnt = 0;
            } else if (bestValue >= beta) {
                alpha = std::max(beta - delta, alpha);
                beta = std::min(bestValue + delta, VALUE_INFINITE);
                ++failedHighCnt;
            } else
                break;
            delta += 47 * delta / 128;
        }
        // Fail high yüzünden sığ arandıysa derinliği yavaş yavaş geri getir
        if (failedHighCnt > 0) failHighRecovery = (failedHighCnt + 1) / 2 + 2;

        if (!Threads.stop) completedDepth = rootDepth;

        if (is_main() && !Threads.silent && !Threads.stop) {
            const auto& rm = rootMoves[0];
            const TimePoint el = std::max<TimePoint>(Time.elapsed(), 1);
            const uint64_t n = Threads.nodes_searched();
            std::cout << "info depth " << rootDepth << " seldepth " << rm.selDepth << " score "
                      << uci_score(rm.uciScore, rootPos)
                      << (rm.inexactLower ? " lowerbound" : rm.inexactUpper ? " upperbound" : "") << " nodes " << n
                      << " nps " << n * 1000 / el << " hashfull " << TT.hashfull() << " time " << el << " pv"
                      << pv_string(rm.pv) << std::endl;
        }

        // Önceki iterasyonda bulunan (daha kısa) matı unutma
        const bool forgottenMate = lastBestMoveScore != -VALUE_INFINITE && is_mate_or_mated(lastBestMoveScore)
                                && (std::abs(rootMoves[0].score) < std::abs(lastBestMoveScore)
                                    || rootMoves[0].is_inexact());
        if (!Threads.stop) {
            if (lastBestMovePV.empty() || lastBestMovePV[0] != rootMoves[0].pv[0]) lastBestMoveDepth = rootDepth;
            if (!forgottenMate) {
                lastBestMovePV = rootMoves[0].pv;
                lastBestMoveScore = rootMoves[0].score;
            }
        }

        // Yarıda kalan aramanın kesin kayıp skoruna güvenme: önceki iterasyona dön
        const bool abortedLossSearch = Threads.stop && rootMoves[0].is_exact_loss();
        if (abortedLossSearch || (rootMoves[0].score != -VALUE_INFINITE && forgottenMate)) {
            if (!lastBestMovePV.empty()) {
                auto it = std::find(rootMoves.begin(), rootMoves.end(), lastBestMovePV[0]);
                if (it != rootMoves.end()) std::rotate(rootMoves.begin(), it, it + 1);
                rootMoves[0].score = rootMoves[0].uciScore = lastBestMoveScore;
                rootMoves[0].pv = lastBestMovePV;
                rootMoves[0].unset_inexact();
            } else if (abortedLossSearch)
                rootMoves[0].inexactLower = true;
        }

        // İstenen mat bulunduysa dur
        if (limits.mate && !Threads.stop && is_mate_or_mated(rootMoves[0].score)
            && VALUE_MATE - std::abs(rootMoves[0].score) <= 2 * limits.mate)
            Threads.stop = true;

        if (!is_main()) continue;

        totBestMoveChanges += Threads.take_best_move_changes();

        // Zaman: skor düşüyorsa / en iyi hamle sık değişiyorsa daha çok düşün,
        // en iyi hamle aramanın çoğunu alıyorsa (açık ara en iyi) daha az
        if (limits.use_time() && !Threads.stop) {
            const uint64_t nodesEffort = rootMoves[0].effort * 100000 / std::max<uint64_t>(1, nodes.load());

            double fallingEval = (11.48 + 2.30 * (bestPreviousAverageScore - bestValue)
                                  + 1.1 * (iterValue[iterIdx] - bestValue))
                               / 100.0;
            fallingEval = std::clamp(fallingEval, 0.576, 1.728);

            timeReduction =
                std::clamp(interpolate(double(rootDepth - lastBestMoveDepth), 4.96, 18.79, 0.639, 1.712), 0.629, 1.544);
            const double reduction = (1.468 + previousTimeReduction) / (2.284 * timeReduction);
            const double bestMoveInstability = 1.077 + 2.229 * totBestMoveChanges / double(Threads.size());
            const double highBestMoveEffort =
                std::clamp(interpolate(int64_t(nodesEffort), int64_t(75800), int64_t(104510), 0.969, 0.714), 0.693, 0.838);

            double totalTime = double(Time.optimum()) * fallingEval * reduction * bestMoveInstability * highBestMoveEffort;
            if (rootMoves.size() == 1) totalTime = std::min(500.0, totalTime);

            const TimePoint el = Time.elapsed();
            if (el > std::min(totalTime, double(Time.maximum())) || rootMoves[0].score >= mate_in(3)
                || rootMoves[0].score == mated_in(2))
                Threads.stop = true;
            else
                Threads.increaseDepth = el <= totalTime * 0.50;
        }
        iterValue[iterIdx] = bestValue;
        iterIdx = (iterIdx + 1) & 3;
    }
    if (is_main()) previousTimeReduction = timeReduction;
}

// ---------------------------------------------------------------------------
//  Yardımcılar
// ---------------------------------------------------------------------------
Value Thread::evaluate(const Position& pos) {
    return Eval::evaluate(pos, evaluator, optimism[pos.side_to_move()]);
}

int Thread::reduction(bool improving, Depth d, int mn, int delta) const {
    const int reductionScale = reductions[d] * reductions[mn];
    return reductionScale - delta * 577 / rootDelta + !improving * reductionScale * 197 / 512 + 982;
}

// Statik değerlendirmeye eklenecek düzeltme (1/131072 ölçeğinde)
int Thread::correction_value(const Position& pos, const Search::Stack* ss) {
    const Color us = pos.side_to_move();
    const Move m = (ss - 1)->currentMove;
    const int pcv = correctionHistory.entry(pos.pawn_key())[us].pawn;
    const int micv = correctionHistory.entry(pos.minor_piece_key())[us].minor;
    const int wnpcv = correctionHistory.entry(pos.non_pawn_key(WHITE))[us].nonPawnWhite;
    const int bnpcv = correctionHistory.entry(pos.non_pawn_key(BLACK))[us].nonPawnBlack;
    const Piece pc = m.is_ok() ? pos.piece_on(m.to_sq()) : NO_PIECE;
    const int cntcv = m.is_ok() ? 7885
                                      * ((*(ss - 2)->continuationCorrectionHistory)(pc, m.to_sq())
                                         + (*(ss - 4)->continuationCorrectionHistory)(pc, m.to_sq()))
                                  + 6307 * (*(ss - 6)->continuationCorrectionHistory)(pc, m.to_sq())
                                : 80695;
    return 13806 * pcv + 9512 * micv + 11615 * (wnpcv + bnpcv) + cntcv;
}

void Thread::update_correction_history(const Position& pos, Search::Stack* ss, int bonus) {
    const Move m = (ss - 1)->currentMove;
    const Color us = pos.side_to_move();
    constexpr int nonPawnWeight = 186;

    correctionHistory.entry(pos.pawn_key())[us].pawn << bonus;
    correctionHistory.entry(pos.minor_piece_key())[us].minor << bonus * 150 / 128;
    correctionHistory.entry(pos.non_pawn_key(WHITE))[us].nonPawnWhite << bonus * nonPawnWeight / 128;
    correctionHistory.entry(pos.non_pawn_key(BLACK))[us].nonPawnBlack << bonus * nonPawnWeight / 128;

    if (m.is_ok()) {
        const Square to = m.to_sq();
        const Piece pc = pos.piece_on(to);
        (*(ss - 2)->continuationCorrectionHistory)(pc, to) << bonus * 130 / 128;
        (*(ss - 4)->continuationCorrectionHistory)(pc, to) << bonus * 70 / 128;
        (*(ss - 6)->continuationCorrectionHistory)(pc, to) << bonus * 35 / 128;
    }
}

void Thread::do_move(Position& pos, Move m, Search::Stack* ss) {
    TT.prefetch(pos.prefetch_key(m));
    const bool capture = pos.capture_stage(m);
    const Piece pc = pos.moved_piece(m);
    nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    pos.do_move(m);
    ss->currentMove = m;
    ss->continuationHistory = &continuationHistory[ss->inCheck][capture](pc, m.to_sq());
    ss->continuationCorrectionHistory = &continuationCorrectionHistory(pc, m.to_sq());
}

void Thread::do_null_move(Position& pos, Search::Stack* ss) {
    TT.prefetch(pos.key() ^ Zobrist::side
                ^ (pos.ep_square() != SQ_NONE ? Zobrist::enpassant[file_of(pos.ep_square())] : 0));
    pos.do_null_move();
    ss->currentMove = Move::null();
    ss->continuationHistory = &continuationHistory[0][0](NO_PIECE, SQ_A1);
    ss->continuationCorrectionHistory = &continuationCorrectionHistory(NO_PIECE, SQ_A1);
}

// ---------------------------------------------------------------------------
//  Ana arama
// ---------------------------------------------------------------------------
template <int NT>
Value Thread::search(Position& pos, Search::Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
    using namespace Search;
    constexpr bool PvNode = NT != NonPV;
    constexpr bool rootNode = NT == Root;
    const bool allNode = !(PvNode || cutNode);
    // Mat arama modu: kök skoru derinliğe göre düşen bir eşiği geçtiyse (ayar değil, ayarlanmaz)
    const bool seekMate = std::abs(rootMoves[pvIdx].score) >= 750 + 220000 / (rootDepth * rootDepth);

    if (depth <= 0) return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    // Uzatmalar derinliği fazla büyüttüyse sınırla
    depth = std::min(depth, MAX_PLY - 1);

    // Tek hamlede tekrar çekilebiliyorsa beraberlik alt sınırdır
    if (!rootNode && alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply)) {
        alpha = value_draw(nodes.load(std::memory_order_relaxed));
        if (alpha >= beta) return alpha;
    }

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY + 1], capturesSearched[SEARCHEDLIST_CAPACITY], quietsSearched[SEARCHEDLIST_CAPACITY];
    int captureCount = 0, quietCount = 0;
    Move move, excludedMove, bestMove;
    Depth extension, newDepth;
    Value bestValue, value, eval, maxValue, probCutBeta;
    bool givesCheck, improving, priorCapture, opponentWorsening, capture, ttCapture;
    int priorReduction;
    Piece movedPiece;

    // 1. Düğümü hazırla
    ss->inCheck = pos.checkers();
    priorCapture = pos.captured_piece();
    const Color us = pos.side_to_move();
    ss->moveCount = 0;
    bestValue = -VALUE_INFINITE;
    maxValue = VALUE_INFINITE;

    ss->followPV = rootNode
                || ((ss - 1)->followPV && size_t(ss->ply - 1) < lastIterationIdxPV.size()
                    && (ss - 1)->currentMove == lastIterationIdxPV[ss->ply - 1]);

    if (is_main()) check_time();
    if (PvNode && selDepth < ss->ply + 1) selDepth = ss->ply + 1;

    if (!rootNode) {
        // 2. Durma, beraberlik, en derin ply
        if (Threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : value_draw(nodes.load(std::memory_order_relaxed));

        // 3. Mat mesafesi budaması: daha kısa bir mat zaten bulunduysa bu dal onu geçemez
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply + 1), beta);
        if (alpha >= beta) return alpha;
    }

    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
    bestMove = Move::none();
    priorReduction = (ss - 1)->reduction;
    (ss - 1)->reduction = 0;
    ss->statScore = 0;
    (ss + 2)->cutoffCnt = 0;
    (ss + 1)->priorNMPFailHigh = 0;

    const int correctionValue = correction_value(pos, ss);

    // 4. TT
    excludedMove = ss->excludedMove;
    const Key posKey = pos.key();
    bool ttHit;
    TTData ttData;
    TTEntry* tte = TT.probe(posKey, ttHit, ttData);
    ss->ttHit = ttHit;
    ttData.move = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50()) : VALUE_NONE;
    ss->ttPv = excludedMove ? ss->ttPv : PvNode || (ttHit && ttData.isPv);
    ttCapture = ttData.move && pos.capture_stage(ttData.move);

    // 5. Statik değerlendirme (correction history ile düzeltilmiş)
    Value unadjustedStaticEval = VALUE_NONE;
    if (ss->inCheck)
        ss->staticEval = eval = (ss - 2)->staticEval;
    else if (excludedMove)
        unadjustedStaticEval = eval = ss->staticEval;
    else if (ss->ttHit) {
        unadjustedStaticEval = ttData.eval;
        if (!is_valid(unadjustedStaticEval)) unadjustedStaticEval = evaluate(pos);
        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
        // TT değeri daha iyi bir tahmin olabilir
        if (is_valid(ttData.value) && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    } else {
        unadjustedStaticEval = evaluate(pos);
        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
        // TT'ye düzeltilmemiş değer yazılır
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(), unadjustedStaticEval);
    }

    // Değerlendirme bizim için iki ply öncesinden iyi mi? Rakip için önceki plydan kötü mü?
    improving = ss->staticEval > (ss - 2)->staticEval;
    opponentWorsening = ss->staticEval > -(ss - 1)->staticEval;

    // Önceki indirgemeyi değerlendirme farkına göre geriye dönük düzelt
    if (priorReduction >= 3 && !opponentWorsening) depth++;
    if (priorReduction >= 2 && depth >= 2 && ss->staticEval + (ss - 1)->staticEval > 166) depth--;

    // 6. TT kesmesi (PV olmayan düğümde, yeterince derin kayıt)
    if (!PvNode && !excludedMove && is_valid(ttData.value) && ttData.depth > depth - (ttData.value <= beta)) {
        // Kayıt kesme yapabiliyor
        if ((ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
            && (cutNode == (ttData.value >= beta) || depth > 4)) {
            if (ttData.move && ttData.value >= beta) {
                // Beta'yı geçen sessiz TT hamlesine bonus
                if (!ttCapture) update_quiet_histories(pos, ss, ttData.move, 131 * depth);
                // Önceki ply'ın erken sessiz hamlesine ek ceza
                if (prevSq != SQ_NONE && (ss - 1)->moveCount < 5 && !priorCapture)
                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2210);
            }

            // 50 hamle sayacı yüksekse kesme yapma (graph history interaction)
            if (pos.rule50() < 96) {
                if (depth >= 7 && ttData.move && pos.legal_move(ttData.move) && !is_decisive(ttData.value)) {
                    // TT hamlesinden sonraki kayıt da aynı kesmeyi destekliyor mu?
                    pos.do_move(ttData.move);
                    bool hitNext;
                    TTData dataNext;
                    TT.probe(pos.key(), hitNext, dataNext);
                    pos.undo_move(ttData.move);
                    if (!is_valid(dataNext.value)) return ttData.value;
                    if ((ttData.value >= beta) == (-dataNext.value >= beta)) return ttData.value;
                } else
                    return ttData.value;
            }
        }
        // Derinlik yetiyordu ama sınır yönü uymadı: kayıt artık işe yaramıyor
        else if (ttData.bound != BOUND_EXACT && (ttData.bound & (ttData.value >= beta ? BOUND_UPPER : BOUND_LOWER))
                 && depth > 5)
            tte->penalize(1);
    }

    if (ss->inCheck) goto moves_loop;

    // Statik değerlendirme farkıyla önceki sessiz hamlenin sıralamasını düzelt
    if ((ss - 1)->currentMove.is_ok() && !(ss - 1)->inCheck && !priorCapture) {
        const int evalDiff = std::clamp(-int((ss - 1)->staticEval + ss->staticEval), -189, 194) + 60;
        mainHistory(~us, (ss - 1)->currentMove) << evalDiff * 11;
        if (!ttHit && type_of(pos.piece_on(prevSq)) != PAWN && (ss - 1)->currentMove.type_of() != PROMOTION)
            pawnHistory[pos](pos.piece_on(prevSq), prevSq) << evalDiff * 13;
    }

    // 8. Razoring: çok kötüyse direkt qsearch
    if (allNode && eval < alpha - 342 * depth && !seekMate) return qsearch<NonPV>(pos, ss, alpha, beta);

    // 9. Reverse futility: çok iyiyse keser
    if (!ss->ttPv && depth < (seekMate ? 6 : 19) && eval >= beta && (!ttData.move || ttCapture) && !is_loss(beta)
        && !is_win(eval)) {
        int futilityMult = std::min(45 + depth * 4, 85);
        futilityMult -= 20 * !ss->ttHit;
        const int futilityMargin = futilityMult * depth
                                 - (2789 * improving + 335 * opponentWorsening) * futilityMult / 1024
                                 + std::abs(correctionValue) / 198435;
        if (eval - futilityMargin >= beta) return (661 * beta + 363 * eval) / 1024;
    }

    // 10. Null move (derinde doğrulama araması ile). Aynı ply'da önceki null move'lar
    // tuttuysa koşul gevşer.
    if (cutNode && ss->staticEval + 50 * ss->priorNMPFailHigh >= beta - 13 * depth - 47 * improving + 365
        && !excludedMove && pos.non_pawn_material(us) && ss->ply >= nmpMinPly && beta >= -2000) {
        const Depth R = 7 + depth / 3 + std::max((ss->staticEval - beta) / 256, 0);
        do_null_move(pos, ss);
        Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
        pos.undo_null_move();

        // Doğrulanmamış mat skoru döndürme
        if (nullValue >= beta && !is_win(nullValue)) {
            if (nmpMinPly || depth < 16) {
                ++ss->priorNMPFailHigh;
                return nullValue;
            }

            // Derin düğümde zugzwang'a karşı doğrulama: nmpMinPly'a kadar null move yok
            nmpMinPly = ss->ply + 3 * (depth - R) / 4;
            Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
            nmpMinPly = 0;
            if (v >= beta) {
                ++ss->priorNMPFailHigh;
                return nullValue;
            }
        }
    }

    improving |= ss->staticEval >= beta;

    // 11. Internal iterative reduction: TT hamlesi olmayan PV/cut düğümünü biraz sığ ara
    if (!ss->followPV && !allNode && depth >= 6 && !ttData.move) depth--;

    // 12. ProbCut: iyi bir alış sığ aramada beta'yı rahatça geçiyorsa kes
    probCutBeta = beta + 241 - 64 * improving;
    if (depth >= 3 && !is_decisive(beta) && !(is_valid(ttData.value) && ttData.value < probCutBeta)) {
        MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval, &captureHistory);
        const Depth probCutDepth = depth - (improving ? 5 : 3);

        while ((move = mp.next_move())) {
            if (move == excludedMove) continue;

            do_move(pos, move, ss);
            // Önce qsearch ile hamlenin tuttuğunu doğrula
            value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);
            if (value >= probCutBeta && probCutDepth > 0)
                value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, probCutDepth, !cutNode);
            undo_move(pos, move);

            if (value >= probCutBeta) {
                tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER, probCutDepth + 1, move,
                          unadjustedStaticEval);
                if (!is_decisive(value)) return value - (probCutBeta - beta);
            }
        }
    }

moves_loop:
    // 13. Küçük ProbCut: TT'deki alt sınır zaten çok yüksek
    probCutBeta = beta + 428;
    if ((ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4 && is_valid(ttData.value)
        && ttData.value >= probCutBeta && !is_decisive(beta) && !is_decisive(ttData.value))
        return probCutBeta;

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory, (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory, (ss - 4)->continuationHistory,
                                        (ss - 5)->continuationHistory, (ss - 6)->continuationHistory};

    MovePicker mp(pos, ttData.move, depth, &mainHistory, &lowPlyHistory, &captureHistory, contHist, &pawnHistory,
                  ss->ply);

    value = bestValue;
    int moveCount = 0;

    // 14. Hamle döngüsü
    while ((move = mp.next_move())) {
        if (move == excludedMove) continue;
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move)) continue;

        ss->moveCount = ++moveCount;
        if (PvNode) (ss + 1)->pv = nullptr;

        extension = 0;
        capture = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);
        newDepth = depth - 1;

        const int delta = beta - alpha;
        int r = reduction(improving, depth, moveCount, delta);

        // TT PV düğümlerinde indirgemeyi artır (aşağıda geri alınır)
        if (ss->ttPv) r += 929;

        // 15. Sığ derinlikte budama
        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue)) {
            // Hamle sayısı eşiği aşıldıysa sessiz hamleleri atla
            if (moveCount >= (3 + depth * depth) / (2 - improving)) mp.skip_quiet_moves();

            int lmrDepth = newDepth - r / 1024;

            if (capture || givesCheck) {
                const Piece capturedPiece = pos.piece_on(move.to_sq());
                const int captHist = captureHistory(movedPiece, move.to_sq(), type_of(capturedPiece));

                // Alışlarda futility
                if (!givesCheck && lmrDepth < 8) {
                    const Value futilityValue = ss->staticEval + 234 + 247 * lmrDepth + PieceValue[capturedPiece]
                                              + 134 * captHist / 1024;
                    if (futilityValue <= alpha) continue;
                }

                // SEE budaması (son taşımızı pat için feda etmeyi budama)
                const int margin = 177 * depth + captHist * 34 / 1024;
                if ((alpha >= VALUE_DRAW || pos.non_pawn_material(us) != PieceValue[movedPiece])
                    && !pos.see_ge(move, -margin))
                    continue;
            } else if (!ss->followPV || !PvNode) {
                int history = (*contHist[0])(movedPiece, move.to_sq()) + (*contHist[1])(movedPiece, move.to_sq())
                            + pawnHistory[pos](movedPiece, move.to_sq());

                // Continuation history budaması
                if (history < -4136 * depth) continue;

                history += 69 * mainHistory(us, move) / 32;
                lmrDepth += history / lmr_divisor(depth);

                const Value futilityValue = ss->staticEval + 119 * lmrDepth + 90 * (ss->staticEval > alpha) + 164;

                // Sessiz hamlelerde futility
                if (!ss->inCheck && lmrDepth < 12 && futilityValue <= alpha) {
                    if (bestValue <= futilityValue && !is_decisive(bestValue) && !is_win(futilityValue))
                        bestValue = futilityValue;
                    continue;
                }

                lmrDepth = std::max(lmrDepth, 0);

                // SEE budaması
                if (!pos.see_ge(move, -23 * lmrDepth * lmrDepth)) continue;
            }
        }

        // 16. Tekil uzatma: TT hamlesi diğer tüm hamlelerden belirgin iyiyse onu derin ara
        if (!rootNode && move == ttData.move && !excludedMove && depth >= 6 + ss->ttPv && is_valid(ttData.value)
            && !is_decisive(ttData.value) && (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 3
            && !is_shuffling(move, ss, pos) && !seekMate) {
            const Value singularBeta = ttData.value - (59 + 66 * (ss->ttPv && !PvNode)) * depth / 63;
            const Depth singularDepth = newDepth / 2;

            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = Move::none();

            if (value < singularBeta) {
                const int corrValAdj = std::abs(correctionValue) / 198368;
                const int doubleMargin = -2 + 204 * PvNode - 152 * !ttCapture - corrValAdj
                                       - 1175 * ttMoveHistory / 114178 - (ss->ply > rootDepth) * 38;
                const int tripleMargin = 70 + 279 * PvNode - 188 * !ttCapture + 81 * ss->ttPv - corrValAdj
                                       - (ss->ply > rootDepth) * 43;
                extension = 1 + (value < singularBeta - doubleMargin) + (value < singularBeta - tripleMargin);
                depth++;
            }
            // Çoklu kesme: TT hamlesi olmadan da beta geçiliyor
            else if (value >= beta && !is_decisive(value)) {
                ttMoveHistory << -421 - 110 * depth;
                if (!ss->inCheck && value > ss->staticEval) {
                    const int bonus = std::clamp(int(value - ss->staticEval) * 664 / 1024,
                                                 -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
                    update_correction_history(pos, ss, bonus);
                }
                return value;
            }
            // Negatif uzatma
            else if (ttData.value >= beta || cutNode)
                extension = -3;
        }

        const uint64_t nodeCount = rootNode ? nodes.load(std::memory_order_relaxed) : 0;

        // 17. Hamleyi yap
        do_move(pos, move, ss);
        newDepth += extension;

        // 18. LMR (indirgeme 1/1024 ölçeğinde)
        if (ss->ttPv)
            r -= 3023 + PvNode * 1004 + (ttData.value > alpha) * 885 + (ttData.depth >= depth) * (816 + cutNode * 940);

        r += 697;
        r -= moveCount * 65;
        r -= std::abs(correctionValue) / 26310;

        if (cutNode) r += 4026 + 933 * !ttData.move;
        if (ttCapture) r += 1079;
        // Sonraki ply'da çok kesme olduysa
        if ((ss + 1)->cutoffCnt > 1) r += 264 + 1095 * ((ss + 1)->cutoffCnt > 2) + 1138 * allNode;
        else if (move == ttData.move) r -= 2179;

        if (capture)
            ss->statScore = 873 * int(PieceValue[pos.captured_piece()]) / 128
                          + captureHistory(movedPiece, move.to_sq(), type_of(pos.captured_piece()));
        else
            ss->statScore = (2252 * mainHistory(us, move) + 1126 * (*contHist[0])(movedPiece, move.to_sq())
                             + 1093 * (*contHist[1])(movedPiece, move.to_sq()))
                          / 1024;

        // History'si iyi hamleyi daha az, kötüsünü daha çok indirge
        r -= ss->statScore * 439 / 4096;

        if (!capture && !is_decisive(alpha)) r += 3 * std::clamp(alpha - eval, -64, 96);

        // Beklenen all düğümlerinde indirgemeyi büyüt
        if (allNode) r += r * 276 / (256 * depth + 268);

        if (depth >= 2 && moveCount > 1) {
            // Negatif indirgemede sınırlı bir uzatmaya izin ver (ağacın çok derininde değil)
            const Depth d = std::max(1, newDepth + std::min(-r / 1024, ss->ply < 2 * rootDepth ? 2 : 0)) + PvNode;

            ss->reduction = newDepth - d;
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);
            ss->reduction = 0;

            // İndirgenmiş arama alpha'yı geçtiyse tam derinlikte tekrar
            if (value > alpha) {
                const bool doDeeperSearch = d < newDepth && value > bestValue + 53;
                const bool doShallowerSearch = value < bestValue + 8;
                newDepth += doDeeperSearch - doShallowerSearch;

                if (newDepth > d) value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                update_continuation_histories(ss, movedPiece, move.to_sq(), 1334);
            }
        }
        // 19. LMR yoksa tam derinlik (sıfır pencere)
        else if (!PvNode || moveCount > 1) {
            if (!ttData.move) r += 1127;
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha,
                                   newDepth - (r > 5234) - (r > 5487 && newDepth > 2), !cutNode);
        }

        // 20. PV düğümünde ilk hamle ya da alpha'yı geçen hamle: tam pencere
        if (PvNode && (moveCount == 1 || value > alpha)) {
            (ss + 1)->pv = pv;
            (ss + 1)->pv[0] = Move::none();

            // qsearch'e dalmak üzereysek TT hamlesini uzat
            if (move == ttData.move
                && ((is_valid(ttData.value) && is_decisive(ttData.value) && ttData.depth > 0) || ttData.depth > 1))
                newDepth = std::max(newDepth, 1);

            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        // 21. Geri al
        undo_move(pos, move);

        // 22. Durduysak sonuç güvenilmez
        if (Threads.stop.load(std::memory_order_relaxed)) return VALUE_ZERO;

        if (rootNode) {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            const uint64_t N = nodes.load(std::memory_order_relaxed) - nodeCount;
            rm.effort += N;
            const uint64_t E_prev = std::max<uint64_t>(1, rm.effort - N);

            // Kök hamlesinin skorları için üstel hareketli ortalama (ağırlık aramanın payına göre)
            constexpr uint64_t Scale = 32, ChiNumerator = 3, ChiDenominator = 2, MinWeight = 12, MaxWeight = 24;
            const uint64_t w =
                std::clamp((Scale * N * ChiDenominator) / (N * ChiDenominator + ChiNumerator * E_prev), MinWeight, MaxWeight);
            const uint64_t w_mss = std::min<uint64_t>(w, 16);
            const int64_t v2 = int64_t(value) * std::abs(value);

            if (rm.averageScore == -VALUE_INFINITE) rm.averageScore = value;
            else rm.averageScore = Value((value * int64_t(w) + rm.averageScore * int64_t(Scale - w)) / int64_t(Scale));

            if (rm.meanSquaredScore == -VALUE_INFINITE * VALUE_INFINITE) rm.meanSquaredScore = value * std::abs(value);
            else
                rm.meanSquaredScore =
                    Value((v2 * int64_t(w_mss) + int64_t(rm.meanSquaredScore) * int64_t(Scale - w_mss)) / int64_t(Scale));

            if (moveCount == 1 || value > alpha) {
                rm.score = rm.uciScore = value;
                rm.selDepth = selDepth;
                rm.unset_inexact();
                if (value >= beta) {
                    rm.inexactLower = true;
                    rm.uciScore = beta;
                } else if (value <= alpha) {
                    rm.inexactUpper = true;
                    rm.uciScore = alpha;
                }
                rm.pv.resize(1);
                for (Move* m = (ss + 1)->pv; m && *m; ++m) rm.pv.push_back(*m);
                if (moveCount > 1 && !pvIdx) ++bestMoveChanges;
            } else
                rm.score = -VALUE_INFINITE;  // sıralamada sona (sıralama kararlı)
        }

        // Eşit değerli alternatifi bazen en iyi hamle yap (çeşitlilik)
        const int inc = (value == bestValue && ss->ply + 2 >= rootDepth
                         && (int(nodes.load(std::memory_order_relaxed)) & 14) == 0 && !is_win(std::abs(value) + 1));

        if (value + inc > bestValue) {
            bestValue = value;
            if (value + inc > alpha) {
                bestMove = move;
                if (PvNode && !rootNode) update_pv(ss->pv, move, (ss + 1)->pv);  // fail high'da da

                if (value >= beta) {
                    ss->cutoffCnt += (extension < 2) || PvNode;
                    break;  // beta kesmesi
                }

                // Bir iyileşme bulduysak kalan hamleleri daha sığ ara
                if (depth > 3 && depth < 12 && !is_decisive(value)) depth -= 3;

                alpha = value;
            }
        }

        // En iyi olmayan hamleleri sonra istatistik için hatırla
        if (move != bestMove && moveCount <= SEARCHEDLIST_CAPACITY) {
            if (capture) capturesSearched[captureCount++] = move;
            else quietsSearched[quietCount++] = move;
        }
    }

    // 23. Mat / pat, istatistikler
    // Fail high'da değeri beta'ya doğru yumuşat
    if (bestValue >= beta && !is_decisive(bestValue) && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;
    else if (bestMove) {
        update_all_stats(pos, ss, bestMove, prevSq, quietsSearched, quietCount, capturesSearched, captureCount, depth,
                         ttData.move, PvNode);
        if (!PvNode) ttMoveHistory << (bestMove == ttData.move ? 918 : -747);
    }
    // Alpha geçilemedi: bu düğüme yol açan sessiz hamle iyi bir cevaptı
    else if (!priorCapture && prevSq != SQ_NONE) {
        int bonusScale = -241;
        bonusScale -= (ss - 1)->statScore / 98;
        bonusScale += std::min(59 * depth, 420);
        bonusScale += 186 * ((ss - 1)->moveCount > 9);
        bonusScale += 142 * (!ss->inCheck && bestValue <= ss->staticEval - 106);
        bonusScale += 159 * (!(ss - 1)->inCheck && bestValue <= -(ss - 1)->staticEval - 68);
        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = std::min(150 * depth - 85, 1337) * bonusScale;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, scaledBonus * 263 / 16384);
        mainHistory(~us, (ss - 1)->currentMove) << scaledBonus * 215 / 32768;
        if (type_of(pos.piece_on(prevSq)) != PAWN && (ss - 1)->currentMove.type_of() != PROMOTION)
            pawnHistory[pos](pos.piece_on(prevSq), prevSq) << scaledBonus * 324 / 8192;
    }
    // Alpha geçilemedi ve önceki hamle alıştı: o alışa bonus
    else if (priorCapture && prevSq != SQ_NONE)
        captureHistory(pos.piece_on(prevSq), prevSq, type_of(pos.captured_piece())) << 892;

    if (PvNode) bestValue = std::min(bestValue, maxValue);

    // İyi hamle yoksa ve önceki pozisyon ttPv idiyse bu pozisyon da PV ağacına girer
    if (bestValue <= alpha) ss->ttPv = ss->ttPv || (ss - 1)->ttPv;

    // 24. TT'ye yaz (düzeltilmemiş statik değerlendirme ile)
    if (!excludedMove && !(rootNode && pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta    ? BOUND_LOWER
                  : PvNode && bestMove ? BOUND_EXACT
                                       : BOUND_UPPER,
                  moveCount != 0 ? depth : std::min(MAX_PLY - 1, depth + 6), bestMove, unadjustedStaticEval);

    // Correction history: en iyi hamle alış değilse ve hata yönü sınırla uyumluysa
    if (!ss->inCheck && !(bestMove && pos.capture(bestMove)) && (bestValue > ss->staticEval) == bool(bestMove)) {
        const int bonus = std::clamp(int(bestValue - ss->staticEval) * depth * (bestMove ? 12 : 18) / 128,
                                     -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(pos, ss, 1061 * bonus / 1024);
    }

    return bestValue;
}

// ---------------------------------------------------------------------------
//  Quiescence
// ---------------------------------------------------------------------------
template <int NT>
Value Thread::qsearch(Position& pos, Search::Stack* ss, Value alpha, Value beta) {
    using namespace Search;
    constexpr bool PvNode = NT == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    // Tek hamlede tekrar çekilebiliyorsa beraberlik alt sınırdır
    if (alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply)) {
        alpha = value_draw(nodes.load(std::memory_order_relaxed));
        if (alpha >= beta) return alpha;
    }

    Move pv[MAX_PLY + 1];
    Move move, bestMove;
    Value bestValue, value, futilityBase;
    bool pvHit, givesCheck, capture;
    int moveCount;

    // 1. Düğümü hazırla
    if (PvNode) {
        (ss + 1)->pv = pv;
        ss->pv[0] = Move::none();
    }
    bestMove = Move::none();
    ss->inCheck = pos.checkers();
    moveCount = 0;

    if (PvNode && selDepth < ss->ply + 1) selDepth = ss->ply + 1;

    // 2. Beraberlik / en derin ply
    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : VALUE_DRAW;

    // 3. TT
    const Key posKey = pos.key();
    bool ttHit;
    TTData ttData;
    TTEntry* tte = TT.probe(posKey, ttHit, ttData);
    ss->ttHit = ttHit;
    ttData.move = ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50()) : VALUE_NONE;
    pvHit = ttHit && ttData.isPv;

    if (!PvNode && ttData.depth >= DEPTH_QS && is_valid(ttData.value)
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttData.value;

    // 4. Statik değerlendirme
    Value unadjustedStaticEval = VALUE_NONE;
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else {
        const int correctionValue = correction_value(pos, ss);
        if (ss->ttHit) {
            unadjustedStaticEval = ttData.eval;
            if (!is_valid(unadjustedStaticEval)) unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            // TT değeri daha iyi bir tahmin olabilir
            if (is_valid(ttData.value) && !is_decisive(ttData.value)
                && (ttData.bound & (ttData.value > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        } else {
            unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
        }

        // Stand pat
        if (bestValue >= beta) {
            if (!is_decisive(bestValue)) bestValue = (441 * bestValue + 583 * beta) / 1024;
            if (!ss->ttHit)
                tte->save(posKey, VALUE_NONE, false, BOUND_LOWER, DEPTH_UNSEARCHED, Move::none(), unadjustedStaticEval);
            return bestValue;
        }
        if (bestValue > alpha) alpha = bestValue;
        futilityBase = ss->staticEval + 306;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory};
    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;

    // Şahta değilsek alışlar, şahtaysak tüm kaçışlar
    MovePicker mp(pos, ttData.move, DEPTH_QS, &mainHistory, &lowPlyHistory, &captureHistory, contHist, &pawnHistory,
                  ss->ply);

    // 5. Hamle döngüsü
    while ((move = mp.next_move())) {
        givesCheck = pos.gives_check(move);
        capture = pos.capture_stage(move);
        moveCount++;

        // 6. Budama
        if (!is_loss(bestValue)) {
            // Futility ve hamle sayısı budaması
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase) && move.type_of() != PROMOTION) {
                if (moveCount > 2) continue;

                const Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];
                // Alınan taşla bile alpha'ya yetişemiyorsa
                if (futilityValue <= alpha) {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }
                // SEE yeterince kötüyse
                if (!pos.see_ge(move, alpha - futilityBase)) {
                    bestValue = std::max(bestValue, std::min(alpha, futilityBase));
                    continue;
                }
            }

            // Alış olmayanları atla
            if (!capture) continue;

            // SEE'si kötü alışları arama
            if (!pos.see_ge(move, -74)) continue;
        }

        // 7. Hamleyi yap ve ara
        do_move(pos, move, ss);
        value = -qsearch<NT>(pos, ss + 1, -beta, -alpha);
        undo_move(pos, move);

        // 8. En iyi hamle
        if (value > bestValue) {
            bestValue = value;
            if (value > alpha) {
                bestMove = move;
                if (PvNode) update_pv(ss->pv, move, (ss + 1)->pv);
                if (value < beta) alpha = value;
                else break;  // fail high
            }
        }
    }

    // 9. Mat / pat
    if (!moveCount) {
        if (ss->inCheck) return mated_in(ss->ply);

        // Pat sadece özel durumda kontrol edilir: piyonlar itilemiyor, taş yok ve son hamle taş aldı
        const Color us = pos.side_to_move();
        const Bitboard pushes = us == WHITE ? shift<NORTH>(pos.pieces(us, PAWN)) : shift<SOUTH>(pos.pieces(us, PAWN));
        if (!(pushes & ~pos.pieces()) && !pos.non_pawn_material(us) && type_of(pos.captured_piece()) >= KNIGHT
            && !MoveList<LEGAL>(pos).size())
            bestValue = VALUE_DRAW;
    }

    if (!is_decisive(bestValue) && bestValue > beta) bestValue = (462 * bestValue + 562 * beta) / 1024;

    // 10. TT'ye yaz (düzeltilmemiş statik değerlendirme ile)
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit, bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS,
              bestMove, unadjustedStaticEval);

    return bestValue;
}

// ---------------------------------------------------------------------------
//  İstatistik güncellemeleri
// ---------------------------------------------------------------------------
void Thread::update_quiet_histories(const Position& pos, Search::Stack* ss, Move move, int bonus) {
    const Color us = pos.side_to_move();
    mainHistory(us, move) << bonus;
    if (ss->ply < LOW_PLY_HISTORY_SIZE) lowPlyHistory(ss->ply, move) << bonus * 712 / 1024;
    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 750 / 1024);
    pawnHistory[pos](pos.moved_piece(move), move.to_sq()) << bonus * (bonus > -4 ? 1104 : 459) / 1024;
}

void Thread::update_all_stats(const Position& pos, Search::Stack* ss, Move bestMove, Square prevSq,
                              const Move* quietsSearched, int quietCount, const Move* capturesSearched,
                              int captureCount, Depth depth, Move ttMove, bool PvNode) {
    Piece movedPiece = pos.moved_piece(bestMove);
    PieceType capturedPiece;

    int bonus = std::min(133 * depth - 81, 1487) + 364 * (bestMove == ttMove) + (ss - 1)->statScore / 28;
    const int malus = std::min(968 * depth - 235, 2244);

    // Çok hamle denendiyse en iyisinin bonusu büyür
    if (!PvNode) bonus += int(bonus * uint64_t(quietCount + captureCount) / 256);

    if (!pos.capture_stage(bestMove)) {
        update_quiet_histories(pos, ss, bestMove, bonus * 899 / 1024);
        // En iyi olmayan sessiz hamlelere azalan ceza
        int actualMalus = malus * 1159 / 1024;
        for (int i = 0; i < quietCount; ++i) {
            actualMalus = actualMalus * 921 / 1024;
            update_quiet_histories(pos, ss, quietsSearched[i], -actualMalus);
        }
    } else {
        capturedPiece = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory(movedPiece, bestMove.to_sq(), capturedPiece) << bonus * 1427 / 1024;
    }

    // Önceki ply'ın erken (TT hamlesi olmayan) sessiz hamlesi burada çürütüldü: ceza
    if (prevSq != SQ_NONE && (ss - 1)->moveCount == 1 + (ss - 1)->ttHit && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 713 / 1024);

    for (int i = 0; i < captureCount; ++i) {
        movedPiece = pos.moved_piece(capturesSearched[i]);
        capturedPiece = type_of(pos.piece_on(capturesSearched[i].to_sq()));
        captureHistory(movedPiece, capturesSearched[i].to_sq(), capturedPiece) << -malus * 1489 / 1024;
    }
}

}  // namespace mai
