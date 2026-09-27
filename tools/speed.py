#!/usr/bin/env python3
"""Motorların arama hızını aynı koşulda ölçer: tek iş parçacığı, Hash 64,
her pozisyonda sabit süre düşünme, son 'info' satırındaki nps.

    python3 tools/speed.py 10 ./maiengine ../opp/inphish5 ...
"""
import os
import sys

import chess
import chess.engine

FENS = [
    chess.STARTING_FEN,
    "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
]

secs = float(sys.argv[1])
for cmd in sys.argv[2:]:
    path = os.path.abspath(cmd)
    eng = chess.engine.SimpleEngine.popen_uci([path], cwd=os.path.dirname(path))
    opts = {k: v for k, v in {"Threads": 1, "Hash": 64}.items() if k in eng.options}
    eng.configure(opts)
    total_nodes = total_time = 0
    for fen in FENS:
        info = eng.analyse(chess.Board(fen), chess.engine.Limit(time=secs))
        total_nodes += info.get("nodes", 0)
        total_time += info.get("time", secs)
    eng.quit()
    print(f"{os.path.basename(cmd)}\t{int(total_nodes / total_time)}", flush=True)
