#!/usr/bin/env python3
"""Rastgele pozisyonlar üretip python-chess ile perft sayılarını hesaplar.

MaiEngine'in hamle üretimini bağımsız bir kütüphaneyle karşılaştırmak için:
    python3 tools/gen_perft_epd.py 600 3 > tools/perft_random.epd
    ./maiengine epd tools/perft_random.epd

Rastgele oyunlar terfi, en passant, rok ve şah durumlarını bol bol içersin diye
hamle seçimi bunlara ağırlık verir. Ayrıca bilinen tuzak pozisyonlar eklenir.
"""
import random
import sys

import chess

TRICKY = [
    # En passant yatay açmaz: alırsan iki piyon birden kalkar, şah açılır
    "8/8/8/K2pP2r/8/8/8/7k w - d6 0 1",
    "8/8/8/KPp4r/8/8/8/7k w - c6 0 1",
    "7k/8/8/r2pP2K/8/8/8/8 w - d6 0 1",
    # En passant ile şahtan kurtulma / çapraz açmaz
    "8/8/8/2k5/3Pp3/8/8/4K3 b - d3 0 1",
    "8/8/8/8/k2Pp2Q/8/8/3K4 b - d3 0 1",
    "8/5bk1/8/2Pp4/8/1K6/8/8 w - d6 0 1",
    "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1",
    # Rok: geçilen kare tehdit altında / hat açık
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "r3k2r/8/8/8/8/8/8/1R2K2R b Kkq - 0 1",
    "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1",
    "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1",
    "4k3/8/8/8/8/8/8/R3K2r w Q - 0 1",
    # Terfi / alışlı terfi / şah
    "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
    "8/P1k5/K7/8/8/8/8/8 w - - 0 1",
    "2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1",
    # Çifte şah
    "4k3/8/8/8/8/8/4q3/3RK2r w - - 0 1",
    "8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1",
    # Pat / mat
    "k7/8/1Q6/8/8/8/8/7K b - - 0 1",
    "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
    "K1k5/8/P7/8/8/8/8/8 w - - 0 1",
]


def perft(board: chess.Board, depth: int) -> int:
    if depth == 1:
        return board.legal_moves.count()
    n = 0
    for m in list(board.legal_moves):
        board.push(m)
        n += perft(board, depth - 1)
        board.pop()
    return n


def random_positions(count: int, rng: random.Random):
    out = set()
    while len(out) < count:
        b = chess.Board()
        plies = rng.randint(4, 140)
        for _ in range(plies):
            moves = list(b.legal_moves)
            if not moves:
                break
            special = [m for m in moves if m.promotion or b.is_en_passant(m) or b.is_castling(m)
                       or b.gives_check(m)]
            caps = [m for m in moves if b.is_capture(m)]
            r = rng.random()
            if special and r < 0.35:
                m = rng.choice(special)
            elif caps and r < 0.6:
                m = rng.choice(caps)
            else:
                m = rng.choice(moves)
            b.push(m)
            # Arada bir yolda kalan pozisyonları da kaydet (en passant fırsatları için)
            if b.ep_square is not None and b.has_legal_en_passant() and rng.random() < 0.5:
                out.add(b.fen())
        if not b.is_game_over():
            out.add(b.fen())
    return list(out)[:count]


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    rng = random.Random(20260927)
    fens = TRICKY + random_positions(count, rng)
    for fen in fens:
        b = chess.Board(fen)
        parts = [fen]
        for d in range(1, depth + 1):
            parts.append(f"D{d} {perft(b, d)}")
        print(" ;".join(parts), flush=True)


if __name__ == "__main__":
    main()
