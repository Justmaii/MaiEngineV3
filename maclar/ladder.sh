#!/bin/bash
# SF19 UCI_Elo merdiveni (inphish'in yöntemiyle aynı: 1+0.01, 20 oyun, renkler değişir)
cd /home/claude/MaiEngineV3
SF=../opp/sf19/src/stockfish
for elo in 3000 3190; do
  python3 tools/match.py --a ./maiengine --a-name MaiV3 --b $SF --b-name "SF19-$elo" \
    --a-opt Hash=64 --b-opt Hash=64 --b-opt UCI_LimitStrength=true --b-opt UCI_Elo=$elo \
    --tc 1+0.01 --games 20 --concurrency 2 --start 40 --out maclar/sf19-elo$elo > maclar/sf19-elo$elo.out 2>&1
done
python3 tools/match.py --a ./maiengine --a-name MaiV3 --b $SF --b-name "SF19-full" \
  --a-opt Hash=64 --b-opt Hash=64 --tc 1+0.01 --games 20 --concurrency 2 --start 40 --out maclar/sf19-full > maclar/sf19-full.out 2>&1
echo BITTI > maclar/ladder.done
