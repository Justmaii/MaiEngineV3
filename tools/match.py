#!/usr/bin/env python3
"""UCI motor maçı: gerçek saat, paralel oyunlar, süreden kaybetme tespiti.

    python3 tools/match.py --a ./maiengine --b ../opp/inphish5 --tc 10+0.1 \
        --games 100 --concurrency 2 --openings tools/openings2.epd --out maclar/v3-vs-inphish5

Her açılış iki kez oynanır (renkler değişir). Saat, motorun cevabı beklenirken
geçen gerçek süreyle düşülür (GUI'deki gibi); saat eksiye düşerse süreden kayıp.
Adjudication (isteğe bağlı): iki motor da 5 hamle boyunca >= 1000 cp derse
kazanç; 40. hamleden sonra 8 hamle boyunca |skor| <= 10 cp ise beraberlik.
"""
import argparse
import math
import multiprocessing as mp
import os
import shlex
import sys
import time

import chess
import chess.engine
import chess.pgn


def parse_tc(tc):
    base, _, inc = tc.partition("+")
    return float(base), float(inc or 0)


def open_engine(cmd, options):
    eng = chess.engine.SimpleEngine.popen_uci(shlex.split(cmd), cwd=os.path.dirname(os.path.abspath(shlex.split(cmd)[0])) or None)
    opts = {k: v for k, v in options.items() if k in eng.options}
    if opts:
        eng.configure(opts)
    return eng


def play_game(args, fen, a_white, game_no):
    base, inc = parse_tc(args.tc)
    A = open_engine(args.a, dict(o.split("=", 1) for o in args.a_opt))
    B = open_engine(args.b, dict(o.split("=", 1) for o in args.b_opt))
    board = chess.Board(fen)
    clocks = {chess.WHITE: base, chess.BLACK: base}
    names = {True: args.a_name, False: args.b_name}
    white_is_a = a_white
    result, reason = None, ""
    win_streak = {chess.WHITE: 0, chess.BLACK: 0}
    lose_streak = {chess.WHITE: 0, chess.BLACK: 0}
    draw_streak = 0
    moves_info = []
    games = {id(A): object(), id(B): object()}
    try:
        while True:
            outcome = board.outcome(claim_draw=True)
            if outcome:
                result = outcome.result()
                reason = outcome.termination.name.lower()
                break
            if board.ply() >= 600:
                result, reason = "1/2-1/2", "max_ply"
                break
            stm = board.turn
            eng = A if (stm == chess.WHITE) == white_is_a else B
            limit = chess.engine.Limit(white_clock=max(clocks[chess.WHITE], 0.001),
                                       black_clock=max(clocks[chess.BLACK], 0.001),
                                       white_inc=inc, black_inc=inc)
            t0 = time.monotonic()
            try:
                r = eng.play(board, limit, game=games[id(eng)], info=chess.engine.INFO_SCORE | chess.engine.INFO_BASIC)
            except (chess.engine.EngineError, chess.engine.EngineTerminatedError) as e:
                result = "0-1" if stm == chess.WHITE else "1-0"
                reason = f"engine_error:{e}"
                break
            used = time.monotonic() - t0
            clocks[stm] -= used
            if clocks[stm] < 0:
                result = "0-1" if stm == chess.WHITE else "1-0"
                reason = "time_forfeit"
                break
            clocks[stm] += inc
            if r.move is None or r.move not in board.legal_moves:
                result = "0-1" if stm == chess.WHITE else "1-0"
                reason = f"illegal_move:{r.move}"
                break
            sc = r.info.get("score")
            cp = sc.pov(stm).score(mate_score=100000) if sc else None
            moves_info.append((r.move, cp, r.info.get("depth"), used))
            board.push(r.move)

            if args.adjudicate and cp is not None:
                # Kazanç: kazanan kendi hamlelerinde >= +1000, kaybeden kendi hamlelerinde <= -1000 (5'er kez üst üste)
                win_streak[stm] = win_streak[stm] + 1 if cp >= 1000 else 0
                lose_streak[stm] = lose_streak[stm] + 1 if cp <= -1000 else 0
                if win_streak[stm] >= 5 and lose_streak[not stm] >= 5:
                    result = "1-0" if stm == chess.WHITE else "0-1"
                    reason = "adjudicated_win"
                    break
                draw_streak = draw_streak + 1 if abs(cp) <= 10 and board.fullmove_number > 40 else 0
                if draw_streak >= 16:
                    result, reason = "1/2-1/2", "adjudicated_draw"
                    break
    finally:
        for e in (A, B):
            try:
                e.quit()
            except Exception:
                pass

    # PGN
    g = chess.pgn.Game()
    g.headers["Event"] = args.event
    g.headers["Round"] = str(game_no)
    g.headers["White"] = args.a_name if white_is_a else args.b_name
    g.headers["Black"] = args.b_name if white_is_a else args.a_name
    g.headers["Result"] = result
    g.headers["FEN"] = fen
    g.headers["SetUp"] = "1"
    g.headers["TimeControl"] = args.tc
    g.headers["Termination"] = reason
    node = g
    for mv, cp, depth, used in moves_info:
        node = node.add_variation(mv)
        node.comment = f"{'' if cp is None else cp / 100:+}/{depth} {used:.3f}s" if cp is not None else f"{used:.3f}s"
    pgn = str(g)

    if result == "1/2-1/2":
        score_a = 0.5
    else:
        white_won = result == "1-0"
        score_a = 1.0 if white_won == white_is_a else 0.0
    loser_is_a = score_a == 0.0
    return game_no, score_a, result, reason, pgn, loser_is_a


def elo_stats(w, d, l):
    n = w + d + l
    if n == 0:
        return 0.0, 0.0
    s = (w + 0.5 * d) / n
    var = (w * (1 - s) ** 2 + d * (0.5 - s) ** 2 + l * (0 - s) ** 2) / n
    se = math.sqrt(var / n) if n > 1 else 0.5

    def to_elo(p):
        p = min(max(p, 1e-3), 1 - 1e-3)
        return -400 * math.log10(1 / p - 1)

    e = to_elo(s)
    lo, hi = to_elo(s - 1.96 * se), to_elo(s + 1.96 * se)
    return e, (hi - lo) / 2


def worker(job):
    args, fen, a_white, game_no = job
    try:
        return play_game(args, fen, a_white, game_no)
    except Exception as e:  # bir oyun çökerse maç devam etsin
        return game_no, None, "*", f"crash:{e}", "", False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--a-name", default="A")
    ap.add_argument("--b-name", default="B")
    ap.add_argument("--a-opt", action="append", default=[], help="Ad=Değer (ör. Hash=64)")
    ap.add_argument("--b-opt", action="append", default=[])
    ap.add_argument("--tc", default="10+0.1")
    ap.add_argument("--games", type=int, default=100)
    ap.add_argument("--concurrency", type=int, default=2)
    ap.add_argument("--openings", default=os.path.join(os.path.dirname(__file__), "openings2.epd"))
    ap.add_argument("--start", type=int, default=0, help="ilk açılış indeksi")
    ap.add_argument("--out", default="maclar/mac")
    ap.add_argument("--event", default="MaiEngine v3 test")
    ap.add_argument("--adjudicate", action="store_true")
    args = ap.parse_args()

    fens = [l.split(";")[0].strip() for l in open(args.openings) if l.strip()]
    fens = [" ".join(f.split()[:4]) + " 0 1" if len(f.split()) == 4 else f for f in fens]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    jobs = []
    for g in range(args.games):
        fen = fens[(args.start + g // 2) % len(fens)]
        jobs.append((args, fen, g % 2 == 0, g + 1))

    w = d = l = 0
    tf_a = tf_b = 0
    t0 = time.time()
    with open(args.out + ".pgn", "a") as pgn_out, open(args.out + ".log", "a") as log, mp.Pool(args.concurrency) as pool:
        for game_no, score_a, result, reason, pgn, loser_is_a in pool.imap_unordered(worker, jobs):
            if score_a is None:
                print(f"oyun {game_no}: ÇÖKTÜ {reason}", flush=True)
                continue
            if score_a == 1: w += 1
            elif score_a == 0: l += 1
            else: d += 1
            if reason == "time_forfeit":
                if loser_is_a: tf_a += 1
                else: tf_b += 1
            pgn_out.write(pgn + "\n\n")
            pgn_out.flush()
            e, err = elo_stats(w, d, l)
            n = w + d + l
            line = (f"[{n}/{args.games}] oyun {game_no}: {result} ({reason})  "
                    f"{args.a_name}: +{w} ={d} -{l}  {100 * (w + 0.5 * d) / n:.1f}%  elo {e:+.0f} ±{err:.0f}  "
                    f"süreden kayıp {args.a_name}:{tf_a} {args.b_name}:{tf_b}  ({time.time() - t0:.0f}s)")
            print(line, flush=True)
            log.write(line + "\n")
            log.flush()


if __name__ == "__main__":
    main()
