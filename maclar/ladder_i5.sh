#!/bin/bash
cd /home/claude/MaiEngineV3
SF=../opp/sf19/src/stockfish
python3 tools/match.py --a ../opp/inphish5 --a-name inphish5 --b $SF --b-name "SF19-3190" \
  --a-opt Hash=64 --b-opt Hash=64 --b-opt UCI_LimitStrength=true --b-opt UCI_Elo=3190 \
  --tc 1+0.01 --games 20 --concurrency 2 --start 40 --out maclar/i5-sf19-elo3190 > maclar/i5-sf19-elo3190.out 2>&1
python3 tools/match.py --a ../opp/inphish5 --a-name inphish5 --b $SF --b-name "SF19-full" \
  --a-opt Hash=64 --b-opt Hash=64 --tc 1+0.01 --games 20 --concurrency 2 --start 40 --out maclar/i5-sf19-full > maclar/i5-sf19-full.out 2>&1
echo BITTI > maclar/ladder_i5.done
