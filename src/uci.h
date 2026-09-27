// MaiEngine v3 — UCI protokolü
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#pragma once

#include <string>

namespace mai::UCI {

// Standart girdiden komut okur (GUI bağlantısı)
void loop(const std::string& exeDir);
// Sabit pozisyonlarda sabit derinlik araması; toplam düğüm = davranış imzası
void bench(int depth, int threads, int hashMb);

}  // namespace mai::UCI
