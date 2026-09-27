// MaiEngine v3 — perft (hamle üretimi doğrulama)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <cstdint>
#include <string>

#include "position.h"

namespace mai::Perft {

uint64_t perft(Position& pos, int depth);
// Kök hamle başına düğüm sayısı (Stockfish "go perft" ile aynı biçim)
uint64_t divide(Position& pos, int depth);
// Ağacı gezerken her düğümde tutarlılık kontrolleri yapar; hata sayısını döndürür
uint64_t verify(Position& pos, int depth, uint64_t& nodes);
// Yerleşik test takımı (bilinen doğru sayılar)
bool run_suite(bool full);
// EPD dosyası: "FEN ;D1 20 ;D2 400 ..." satırları
bool run_epd(const std::string& file, int maxDepth);

}  // namespace mai::Perft
