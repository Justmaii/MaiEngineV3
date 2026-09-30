// MaiEngine v3 — NNUE değerlendirmesi (Stockfish 19 ağı)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "nnue.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace mai::NNUE {

namespace {

constexpr uint32_t Version = 0x6A448AFAu;
constexpr int WeightScaleBits = 6;
constexpr int OutputScale = 16;

// ---------------- Ağırlıklar ----------------
struct LayerStack {
    alignas(64) int32_t b0[L2];
    alignas(64) int8_t w0[L2 * HalfDims];
    alignas(64) int8_t w0T[HalfDims * L2];  // seyrek çarpım için: [4'lü girdi parçası][çıkış][4]
    alignas(64) int32_t b1[L3];
    alignas(64) int8_t w1[L3 * L2 * 2];
    alignas(64) int32_t b2[1];
    alignas(64) int8_t w2[L2 * 2 + L3 * 2];
};

struct Network {
    alignas(64) int16_t ftBias[HalfDims];
    alignas(64) int16_t psqWeight[PsqDims * HalfDims];      // 46 MB
    alignas(64) int8_t otherWeight[OtherDims * HalfDims];   // 66 MB: önce tehditler, sonra piyon çiftleri
    alignas(64) int32_t psqPsqt[PsqDims * PsqtBuckets];
    alignas(64) int32_t otherPsqt[OtherDims * PsqtBuckets];
    LayerStack stacks[LayerStacks];
};

std::unique_ptr<Network> net;
std::string netDescription;

// ---------------- HalfKAv2_hm indeksi ----------------
// Şah vezir kanadındaysa tahta yatay aynalanır; siyah bakışında ayrıca dikey.
inline int psq_orient(Color persp, Square ksq) {
    const bool queenSide = file_of(ksq) <= FILE_D;
    return persp == WHITE ? (queenSide ? 7 : 0) : (queenSide ? 63 : 56);
}

inline int king_bucket_base(Color persp, Square ksq) {
    const int f = file_of(ksq), r = rank_of(ksq);
    const int column = std::min(f, 7 - f);
    const int row = persp == WHITE ? 7 - r : r;
    return (row * 4 + column) * 11 * 64;
}

// Kendi taşı çift, rakibinki tek blok; şahlar ortak blok (10)
inline int piece_base(Color persp, Piece pc) {
    const PieceType pt = type_of(pc);
    if (pt == KING) return 10 * 64;
    return ((pt - 1) * 2 + (color_of(pc) != persp)) * 64;
}

struct FeatureCtx {
    int orient, bucketBase;
    Color persp;
    FeatureCtx(Color p, Square ksq) : orient(psq_orient(p, ksq)), bucketBase(king_bucket_base(p, ksq)), persp(p) {}
    int index(Square s, Piece pc) const { return (int(s) ^ orient) + piece_base(persp, pc) + bucketBase; }
};

// ---------------- FullThreats ve PP_3Wide indeksi ----------------
// Bu iki set şah kovası kullanmaz; sadece yön: şah a-d dikeyindeyse aynalama yok
// (HalfKA'nın tersi!), siyah bakışında dikey çevirme ve renk değişimi.
inline int other_orient(Color persp, Square ksq) { return (file_of(ksq) <= FILE_D ? 0 : 7) ^ (56 * persp); }

// Piyonlar sadece at ve kaleyi tehdit eder (piyon-piyon ilişkisi PP_3Wide'da).
constexpr int NumValidTargets[PIECE_NB] = {0, 4, 10, 8, 8, 10, 0, 0, 0, 4, 10, 8, 8, 10, 0, 0};
// [saldıran tip][hedef tip] -> hedef sırası; -1 = özellik yok
constexpr int ThreatMap[6][6] = {
    {-1, 0, -1, 1, -1, -1},  // piyon
    {0, 1, 2, 3, 4, -1},     // at
    {0, 1, 2, 3, -1, -1},    // fil
    {0, 1, 2, 3, -1, -1},    // kale
    {0, 1, 2, 3, 4, -1},     // vezir
    {-1, -1, -1, -1, -1, -1} // şah
};

// indeks = Lut1[saldıran][hedef][kaynak < hedef kare] + Offsets[saldıran][kaynak] + Lut2[saldıran][kaynak][hedef kare]
uint32_t ThreatLut1[PIECE_NB][PIECE_NB][2];
uint32_t ThreatOffsets[PIECE_NB][SQUARE_NB];
uint8_t ThreatLut2[PIECE_NB][SQUARE_NB][SQUARE_NB];
Bitboard PawnPairBB[SQUARE_NB];

Bitboard pseudo_attacks(Piece pc, Square s) {
    return type_of(pc) == PAWN ? pawn_attacks(color_of(pc), s) : PseudoAttacks[type_of(pc)][s];
}

bool init_tables() {
    constexpr Piece All[12] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                               B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};
    uint32_t pieceSize[PIECE_NB] = {}, pieceBase[PIECE_NB] = {};
    uint32_t cumulative = 0;
    for (Piece pc : All) {
        uint32_t n = 0;
        for (int s = 0; s < SQUARE_NB; ++s) {
            ThreatOffsets[pc][s] = n;
            // Piyon 1. ve 8. yatayda olamaz
            if (type_of(pc) != PAWN || (s >= SQ_A2 && s <= SQ_H7)) n += popcount(pseudo_attacks(pc, Square(s)));
            const Bitboard att = pseudo_attacks(pc, Square(s));
            for (int t = 0; t < SQUARE_NB; ++t) ThreatLut2[pc][s][t] = uint8_t(popcount(((1ULL << t) - 1) & att));
        }
        pieceSize[pc] = n;
        pieceBase[pc] = cumulative;
        cumulative += NumValidTargets[pc] * n;
    }
    for (Piece a : All)
        for (Piece d : All) {
            const PieceType at = type_of(a), dt = type_of(d);
            const bool enemy = (a ^ d) == 8;
            const int map = ThreatMap[at - 1][dt - 1];
            // Aynı tip karşılıklı tehditler bir kez sayılır (kaynak < hedef olan atılır)
            const bool semiExcluded = at == dt && (enemy || at != PAWN);
            const uint32_t feature = pieceBase[a] + (color_of(d) * (NumValidTargets[a] / 2) + map) * pieceSize[a];
            ThreatLut1[a][d][0] = map < 0 ? ThreatDims : feature;
            ThreatLut1[a][d][1] = map < 0 || semiExcluded ? ThreatDims : feature;
        }
    for (int s = 0; s < SQUARE_NB; ++s) {
        const Bitboard f = file_bb(file_of(Square(s)));
        PawnPairBB[s] = (f | shift<EAST>(f) | shift<WEST>(f)) & ~(Rank1BB | Rank8BB) & ~square_bb(Square(s));
    }
    return cumulative == uint32_t(ThreatDims);
}

// Ham tehdit: saldıran | hedef<<4 | kaynak<<8 | hedef kare<<14 (bakıştan bağımsız)
inline uint32_t pack_threat(Piece a, Square from, Square to, Piece d) {
    return uint32_t(a) | uint32_t(d) << 4 | uint32_t(from) << 8 | uint32_t(to) << 14;
}

inline uint32_t threat_index(uint32_t raw, int orient, int swap) {
    const int a = int(raw & 15) ^ swap, d = int((raw >> 4) & 15) ^ swap;
    const int f = int((raw >> 8) & 63) ^ orient, t = int((raw >> 14) & 63) ^ orient;
    return ThreatLut1[a][d][f < t] + ThreatOffsets[a][f] + ThreatLut2[a][f][t];
}

// Pozisyondaki tüm tehditleri sıralı listeye yaz
void collect_threats(const Position& pos, ThreatState& ts) {
    const Bitboard occ = pos.pieces();
    const Bitboard pawnTargets = pos.pieces(KNIGHT, ROOK);
    const Bitboard minorSliderTargets = pos.pieces(PAWN, KNIGHT) | pos.pieces(BISHOP, ROOK);
    const Bitboard queenTargets = minorSliderTargets | pos.pieces(QUEEN);
    int n = 0;
    auto push = [&](Piece a, Square from, Square to) {
        if (n < MaxThreats) ts.list[n++] = pack_threat(a, from, to, pos.piece_on(to));
    };

    const Bitboard wp = pos.pieces(WHITE, PAWN), bp = pos.pieces(BLACK, PAWN);
    for (Bitboard b = shift<NORTH_EAST>(wp) & pawnTargets; b;) { Square to = pop_lsb(b); push(W_PAWN, Square(to - 9), to); }
    for (Bitboard b = shift<NORTH_WEST>(wp) & pawnTargets; b;) { Square to = pop_lsb(b); push(W_PAWN, Square(to - 7), to); }
    for (Bitboard b = shift<SOUTH_WEST>(bp) & pawnTargets; b;) { Square to = pop_lsb(b); push(B_PAWN, Square(to + 9), to); }
    for (Bitboard b = shift<SOUTH_EAST>(bp) & pawnTargets; b;) { Square to = pop_lsb(b); push(B_PAWN, Square(to + 7), to); }

    for (Color c : {WHITE, BLACK})
        for (PieceType pt = KNIGHT; pt < KING; pt = PieceType(pt + 1)) {
            const Piece a = make_piece(c, pt);
            const Bitboard targets = pt == KNIGHT || pt == QUEEN ? queenTargets : minorSliderTargets;
            for (Bitboard b = pos.pieces(c, pt); b;) {
                const Square from = pop_lsb(b);
                for (Bitboard t = attacks_bb(pt, from, occ) & targets; t;) push(a, from, pop_lsb(t));
            }
        }

    std::sort(ts.list, ts.list + n);
    ts.count = n;
    ts.pawns[WHITE] = wp;
    ts.pawns[BLACK] = bp;
    ts.key = pos.key();
}

inline uint32_t pair_index(Color persp, int orient, Color ca, Square a, Color cb, Square b) {
    const uint32_t ia = 48 * (ca ^ persp) + (int(a) ^ orient) - 8;
    const uint32_t ib = 48 * (cb ^ persp) + (int(b) ^ orient) - 8;
    const uint32_t hi = std::max(ia, ib), lo = std::min(ia, ib);
    return ThreatDims + hi * (hi - 1) / 2 + lo;
}

// "Diğer" bloğun (tehdit + piyon çifti) indeks listesi. Eklenen satırın ağırlıkları
// önceden önbelleğe istenir: 66 MB'lık tabloda rastgele erişim, asıl maliyet bekleme.
struct IndexList {
    uint32_t v[2 * MaxThreats + 512];
    int n = 0;
    void push(uint32_t i) {
        __builtin_prefetch(net->otherWeight + size_t(i) * HalfDims);
        v[n++] = i;
    }
};

void active_other(Color persp, Square ksq, const ThreatState& ts, IndexList& out) {
    const int orient = other_orient(persp, ksq), swap = 8 * persp;
    for (int i = 0; i < ts.count; ++i) {
        const uint32_t idx = threat_index(ts.list[i], orient, swap);
        if (idx < uint32_t(ThreatDims)) out.push(idx);
    }
    const Bitboard white = ts.pawns[WHITE], black = ts.pawns[BLACK];
    for (Bitboard b = white; b;) {
        const Square from = pop_lsb(b);
        for (Bitboard p = PawnPairBB[from] & b; p;) out.push(pair_index(persp, orient, WHITE, from, WHITE, pop_lsb(p)));
        for (Bitboard p = PawnPairBB[from] & black; p;) out.push(pair_index(persp, orient, WHITE, from, BLACK, pop_lsb(p)));
    }
    for (Bitboard b = black; b;) {
        const Square from = pop_lsb(b);
        for (Bitboard p = PawnPairBB[from] & b; p;) out.push(pair_index(persp, orient, BLACK, from, BLACK, pop_lsb(p)));
    }
}

// İki pozisyon arasında değişen piyon çiftleri: en az bir üyesi değişmiş çiftler
void pair_changes(Color persp, int orient, Bitboard updW, Bitboard updB, Bitboard pw, Bitboard pb, IndexList& out) {
    const Bitboard unchanged = (pw | pb) & ~(updW | updB);
    for (Bitboard u = updW | updB; u;) {
        const Square a = pop_lsb(u);
        const Bitboard mask = PawnPairBB[a] & (unchanged | u);
        const Color ca = (pb & square_bb(a)) ? BLACK : WHITE;
        for (Bitboard p = pb & mask; p;) out.push(pair_index(persp, orient, ca, a, BLACK, pop_lsb(p)));
        for (Bitboard p = pw & mask; p;) out.push(pair_index(persp, orient, ca, a, WHITE, pop_lsb(p)));
    }
}

void other_changes(Color persp, Square ksq, const ThreatState& before, const ThreatState& after, IndexList& added,
                   IndexList& removed) {
    const int orient = other_orient(persp, ksq), swap = 8 * persp;
    auto emit = [&](uint32_t raw, IndexList& out) {
        const uint32_t idx = threat_index(raw, orient, swap);
        if (idx < uint32_t(ThreatDims)) out.push(idx);
    };
    // İki sıralı listenin farkı
    int i = 0, j = 0;
    while (i < before.count && j < after.count) {
        if (before.list[i] == after.list[j]) ++i, ++j;
        else if (before.list[i] < after.list[j]) emit(before.list[i++], removed);
        else emit(after.list[j++], added);
    }
    while (i < before.count) emit(before.list[i++], removed);
    while (j < after.count) emit(after.list[j++], added);

    const Bitboard bw = before.pawns[WHITE], bb = before.pawns[BLACK];
    const Bitboard aw = after.pawns[WHITE], ab = after.pawns[BLACK];
    if (bw == aw && bb == ab) return;
    pair_changes(persp, orient, aw & ~bw, ab & ~bb, aw, ab, added);
    pair_changes(persp, orient, bw & ~aw, bb & ~ab, bw, bb, removed);
}

// ---------------- Vektör yardımcıları ----------------
// Tüm döngüler sabit uzunlukta; derleyici -O3 ile vektörleştiriyor.
inline void add_row(int16_t* __restrict acc, const int16_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] += w[i];
}
#if !defined(__ARM_NEON) && !defined(__AVX2__)
inline void sub_row(int16_t* __restrict acc, const int16_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] -= w[i];
}
#endif
inline void add_row8(int16_t* __restrict acc, const int8_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] += w[i];
}
inline void sub_row8(int16_t* __restrict acc, const int8_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] -= w[i];
}
inline void add_psqt(int32_t* p, const int32_t* w) {
    for (int k = 0; k < PsqtBuckets; ++k) p[k] += w[k];
}
inline void sub_psqt(int32_t* p, const int32_t* w) {
    for (int k = 0; k < PsqtBuckets; ++k) p[k] -= w[k];
}

void apply_other(int16_t* acc, int32_t* psqt, const IndexList& added, const IndexList& removed) {
    for (int i = 0; i < added.n; ++i) {
        add_row8(acc, net->otherWeight + size_t(added.v[i]) * HalfDims);
        add_psqt(psqt, net->otherPsqt + size_t(added.v[i]) * PsqtBuckets);
    }
    for (int i = 0; i < removed.n; ++i) {
        sub_row8(acc, net->otherWeight + size_t(removed.v[i]) * HalfDims);
        sub_psqt(psqt, net->otherPsqt + size_t(removed.v[i]) * PsqtBuckets);
    }
}

// Bir güncellemede eklenip çıkarılacak satırlar: HalfKA (int16) ve tehdit/piyon çifti (int8)
struct RowDelta {
    const int16_t* add16[64];
    const int16_t* sub16[64];
    int nAdd16 = 0, nSub16 = 0;
    IndexList add8, sub8;
};

// out = in + tüm satır farkları, tek geçişte. Toplamın 128'lik dilimi yazmaçlarda
// kalır, her satır için sadece ağırlık okunur (ayrı ayrı topla-yaz yerine).
void apply_delta(int16_t* out, const int16_t* in, const RowDelta& d) {  // out == in olabilir
    const int8_t* const w8 = net->otherWeight;
#if defined(__ARM_NEON)
    constexpr int Tile = 128;  // 16 x int16x8
    for (int t = 0; t < HalfDims; t += Tile) {
        int16x8_t r[16];
        for (int k = 0; k < 16; ++k) r[k] = vld1q_s16(in + t + 8 * k);
        for (int i = 0; i < d.nAdd16; ++i)
            for (int k = 0; k < 16; ++k) r[k] = vaddq_s16(r[k], vld1q_s16(d.add16[i] + t + 8 * k));
        for (int i = 0; i < d.nSub16; ++i)
            for (int k = 0; k < 16; ++k) r[k] = vsubq_s16(r[k], vld1q_s16(d.sub16[i] + t + 8 * k));
        for (int i = 0; i < d.add8.n; ++i) {
            const int8_t* p = w8 + size_t(d.add8.v[i]) * HalfDims + t;
            for (int k = 0; k < 8; ++k) {
                const int8x16_t w = vld1q_s8(p + 16 * k);
                r[2 * k] = vaddw_s8(r[2 * k], vget_low_s8(w));
                r[2 * k + 1] = vaddw_high_s8(r[2 * k + 1], w);
            }
        }
        for (int i = 0; i < d.sub8.n; ++i) {
            const int8_t* p = w8 + size_t(d.sub8.v[i]) * HalfDims + t;
            for (int k = 0; k < 8; ++k) {
                const int8x16_t w = vld1q_s8(p + 16 * k);
                r[2 * k] = vsubw_s8(r[2 * k], vget_low_s8(w));
                r[2 * k + 1] = vsubw_high_s8(r[2 * k + 1], w);
            }
        }
        for (int k = 0; k < 16; ++k) vst1q_s16(out + t + 8 * k, r[k]);
    }
#elif defined(__AVX2__)
    constexpr int Tile = 128;  // 8 x (16 x int16)
    for (int t = 0; t < HalfDims; t += Tile) {
        __m256i r[8];
        for (int k = 0; k < 8; ++k) r[k] = _mm256_load_si256(reinterpret_cast<const __m256i*>(in + t + 16 * k));
        for (int i = 0; i < d.nAdd16; ++i)
            for (int k = 0; k < 8; ++k)
                r[k] = _mm256_add_epi16(r[k], _mm256_load_si256(reinterpret_cast<const __m256i*>(d.add16[i] + t + 16 * k)));
        for (int i = 0; i < d.nSub16; ++i)
            for (int k = 0; k < 8; ++k)
                r[k] = _mm256_sub_epi16(r[k], _mm256_load_si256(reinterpret_cast<const __m256i*>(d.sub16[i] + t + 16 * k)));
        for (int i = 0; i < d.add8.n; ++i) {
            const int8_t* p = w8 + size_t(d.add8.v[i]) * HalfDims + t;
            for (int k = 0; k < 8; ++k)
                r[k] = _mm256_add_epi16(r[k], _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(p + 16 * k))));
        }
        for (int i = 0; i < d.sub8.n; ++i) {
            const int8_t* p = w8 + size_t(d.sub8.v[i]) * HalfDims + t;
            for (int k = 0; k < 8; ++k)
                r[k] = _mm256_sub_epi16(r[k], _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(p + 16 * k))));
        }
        for (int k = 0; k < 8; ++k) _mm256_store_si256(reinterpret_cast<__m256i*>(out + t + 16 * k), r[k]);
    }
#else
    std::memcpy(out, in, sizeof(int16_t) * HalfDims);
    for (int i = 0; i < d.nAdd16; ++i) add_row(out, d.add16[i]);
    for (int i = 0; i < d.nSub16; ++i) sub_row(out, d.sub16[i]);
    for (int i = 0; i < d.add8.n; ++i) add_row8(out, w8 + size_t(d.add8.v[i]) * HalfDims);
    for (int i = 0; i < d.sub8.n; ++i) sub_row8(out, w8 + size_t(d.sub8.v[i]) * HalfDims);
#endif
}

void apply_delta_psqt(int32_t* out, const int32_t* in, const RowDelta& d, const int32_t* const* addP,
                      const int32_t* const* subP) {
    std::memcpy(out, in, sizeof(int32_t) * PsqtBuckets);
    for (int i = 0; i < d.nAdd16; ++i) add_psqt(out, addP[i]);
    for (int i = 0; i < d.nSub16; ++i) sub_psqt(out, subP[i]);
    for (int i = 0; i < d.add8.n; ++i) add_psqt(out, net->otherPsqt + size_t(d.add8.v[i]) * PsqtBuckets);
    for (int i = 0; i < d.sub8.n; ++i) sub_psqt(out, net->otherPsqt + size_t(d.sub8.v[i]) * PsqtBuckets);
}

// uint8 girdi x int8 ağırlık iç çarpımı (n, 32'nin katı)
inline int32_t dot_u8i8(const uint8_t* __restrict a, const int8_t* __restrict w, int n) {
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi16(1);
    for (int i = 0; i < n; i += 32) {
        __m256i x = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i y = _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i));
        // Girdi <= 127 olduğundan çift toplamı int16'ya sığar (taşma yok)
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_maddubs_epi16(x, y), ones));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
#elif defined(__ARM_NEON) && defined(__ARM_FEATURE_DOTPROD)
    int32x4_t acc0 = vdupq_n_s32(0), acc1 = vdupq_n_s32(0);
    for (int i = 0; i < n; i += 32) {
        // Girdi <= 127: işaretli bayt olarak okumak güvenli
        acc0 = vdotq_s32(acc0, vreinterpretq_s8_u8(vld1q_u8(a + i)), vld1q_s8(w + i));
        acc1 = vdotq_s32(acc1, vreinterpretq_s8_u8(vld1q_u8(a + i + 16)), vld1q_s8(w + i + 16));
    }
    return vaddvq_s32(vaddq_s32(acc0, acc1));
#elif defined(__ARM_NEON)
    int32x4_t acc = vdupq_n_s32(0);
    for (int i = 0; i < n; i += 16) {
        int8x16_t x = vreinterpretq_s8_u8(vld1q_u8(a + i));
        int8x16_t y = vld1q_s8(w + i);
        int16x8_t p0 = vmull_s8(vget_low_s8(x), vget_low_s8(y));
        int16x8_t p1 = vmull_s8(vget_high_s8(x), vget_high_s8(y));
        acc = vpadalq_s16(acc, p0);
        acc = vpadalq_s16(acc, p1);
    }
    return vaddvq_s32(acc);
#else
    int32_t sum = 0;
    for (int i = 0; i < n; ++i) sum += int32_t(a[i]) * w[i];
    return sum;
#endif
}

// İkişerli çarpım: 1024 toplam -> 512 bayt (her bakış için), [0,255]'e kırp, çarp, 512'ye böl
// Girdi iki parçanın toplamı (int16, taşarsa Stockfish'teki gibi sarar)
inline void transform_half(const int16_t* __restrict p, const int16_t* __restrict q, uint8_t* __restrict out) {
    constexpr int H = HalfDims / 2;
#if defined(__ARM_NEON)
    const int16x8_t zero = vdupq_n_s16(0), max = vdupq_n_s16(255);
    auto clip = [&](int i) {
        return vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(vaddq_s16(vld1q_s16(p + i), vld1q_s16(q + i)), zero), max));
    };
    for (int j = 0; j < H; j += 16) {
        // 255 * 255 = 65025 uint16'ya sığar
        const uint16x8_t a0 = clip(j), a1 = clip(j + 8), b0 = clip(H + j), b1 = clip(H + j + 8);
        vst1q_u8(out + j, vcombine_u8(vmovn_u16(vshrq_n_u16(vmulq_u16(a0, b0), 9)), vmovn_u16(vshrq_n_u16(vmulq_u16(a1, b1), 9))));
    }
    return;
#endif
    for (int j = 0; j < H; ++j) {
        unsigned a = unsigned(std::clamp<int>(int16_t(p[j] + q[j]), 0, 255));
        unsigned b = unsigned(std::clamp<int>(int16_t(p[j + H] + q[j + H]), 0, 255));
        out[j] = uint8_t((a * b) / 512);
    }
}

inline uint8_t sqr_clipped(int32_t x, int shift) { return uint8_t(std::min<int64_t>(127, (int64_t(x) * x) >> shift)); }
inline uint8_t clipped(int32_t x, int shift) { return uint8_t(std::clamp(x >> shift, 0, 127)); }

int propagate(const uint8_t* input, int bucket) {
    const LayerStack& ls = net->stacks[bucket];

    // Girdinin çoğu sıfır: sadece sıfır olmayan 4 baytlık parçaların ağırlık sütunlarını ekle
    alignas(64) int32_t fc0[L2];
    uint16_t nnz[HalfDims / 4];
    int nnzCount = 0;
    uint32_t in32[HalfDims / 4];
    std::memcpy(in32, input, HalfDims);
    for (int c = 0; c < HalfDims / 4; ++c) {  // dalsız
        nnz[nnzCount] = uint16_t(c);
        nnzCount += in32[c] != 0;
    }
#if defined(__ARM_NEON) && defined(__ARM_FEATURE_DOTPROD)
    int32x4_t acc[L2 / 4];
    for (int k = 0; k < L2 / 4; ++k) acc[k] = vld1q_s32(ls.b0 + 4 * k);
    for (int i = 0; i < nnzCount; ++i) {
        const int c = nnz[i];
        const int8x16_t x = vreinterpretq_s8_u32(vdupq_n_u32(in32[c]));  // girdi <= 127
        const int8_t* w = ls.w0T + c * (4 * L2);
        for (int k = 0; k < L2 / 4; ++k) acc[k] = vdotq_s32(acc[k], vld1q_s8(w + 16 * k), x);
    }
    for (int k = 0; k < L2 / 4; ++k) vst1q_s32(fc0 + 4 * k, acc[k]);
#elif defined(__AVX2__)
    __m256i acc[L2 / 8];
    const __m256i ones = _mm256_set1_epi16(1);
    for (int k = 0; k < L2 / 8; ++k) acc[k] = _mm256_load_si256(reinterpret_cast<const __m256i*>(ls.b0 + 8 * k));
    for (int i = 0; i < nnzCount; ++i) {
        const int c = nnz[i];
        const __m256i x = _mm256_set1_epi32(int(in32[c]));
        const int8_t* w = ls.w0T + c * (4 * L2);
        for (int k = 0; k < L2 / 8; ++k) {
            const __m256i y = _mm256_load_si256(reinterpret_cast<const __m256i*>(w + 32 * k));
            acc[k] = _mm256_add_epi32(acc[k], _mm256_madd_epi16(_mm256_maddubs_epi16(x, y), ones));
        }
    }
    for (int k = 0; k < L2 / 8; ++k) _mm256_store_si256(reinterpret_cast<__m256i*>(fc0 + 8 * k), acc[k]);
#else
    for (int o = 0; o < L2; ++o) fc0[o] = ls.b0[o];
    for (int i = 0; i < nnzCount; ++i) {
        const int c = nnz[i];
        const int8_t* w = ls.w0T + c * (4 * L2);
        for (int o = 0; o < L2; ++o)
            for (int j = 0; j < 4; ++j) fc0[o] += int32_t(input[4 * c + j]) * w[4 * o + j];
    }
#endif

    // Her katmanın çıkışı iki kez: karesi alınmış ve düz kırpılmış. Hepsi son katmana da gider.
    alignas(64) uint8_t cat[L2 * 2 + L3 * 2];
    for (int i = 0; i < L2; ++i) {
        cat[i] = sqr_clipped(fc0[i], 2 * (WeightScaleBits + 1) + 7);
        cat[L2 + i] = clipped(fc0[i], WeightScaleBits + 1);
    }

    int32_t fc1[L3];
    for (int o = 0; o < L3; ++o) fc1[o] = ls.b1[o] + dot_u8i8(cat, ls.w1 + o * L2 * 2, L2 * 2);
    for (int i = 0; i < L3; ++i) {
        cat[2 * L2 + i] = sqr_clipped(fc1[i], 2 * WeightScaleBits + 7);
        cat[2 * L2 + L3 + i] = clipped(fc1[i], WeightScaleBits);
    }

    const int32_t fc2 = ls.b2[0] + dot_u8i8(cat, ls.w2, L2 * 2 + L3 * 2);
    // fc0'ın son iki çıkışının farkı ağı atlayıp doğrudan sonuca eklenir
    const int32_t fwd = fc2 + fc0[L2 - 2] - fc0[L2 - 1];
    // 1.0 = 128 * 2^6 * 2 nicemli; bunu 600 * OutputScale'e ölçekle
    return int32_t(int64_t(fwd) * (600 * OutputScale) / (128 * (1 << WeightScaleBits) * 2));
}

alignas(64) const int16_t Zero16[HalfDims] = {};

Output output(const int16_t* usPsq, const int16_t* usThr, const int16_t* themPsq, const int16_t* themThr,
              int32_t psqtUs, int32_t psqtThem, int bucket) {
    alignas(64) uint8_t input[HalfDims];
    transform_half(usPsq, usThr, input);
    transform_half(themPsq, themThr, input + HalfDims / 2);
    const int psqt = (psqtUs - psqtThem) / 2;
    return {psqt / OutputScale, propagate(input, bucket) / OutputScale};
}

// ---------------- Yükleme ----------------
template <typename T>
bool read_array(std::istream& in, T* dst, size_t n) {
    in.read(reinterpret_cast<char*>(dst), std::streamsize(sizeof(T) * n));  // dosya little-endian
    return bool(in);
}

uint32_t read_u32(std::istream& in) {
    uint32_t v = 0;
    read_array(in, &v, 1);
    return v;
}

// İşaretli LEB128 sıkıştırılmış blok: "COMPRESSED_LEB128" + bayt sayısı + değerler
template <typename T>
bool read_leb128(std::istream& in, T* dst, size_t n) {
    static constexpr char Magic[] = "COMPRESSED_LEB128";
    char magic[sizeof(Magic) - 1];
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, Magic, sizeof(magic)) != 0) return false;
    const uint32_t bytes = read_u32(in);
    std::vector<uint8_t> buf(bytes);
    if (!read_array(in, buf.data(), bytes)) return false;

    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t result = 0;
        int shift = 0;
        uint8_t byte;
        do {
            if (pos >= buf.size()) return false;
            byte = buf[pos++];
            result |= uint32_t(byte & 0x7F) << (shift % 32);
            shift += 7;
        } while (byte & 0x80);
        if (shift < 32 && (byte & 0x40)) result |= ~((uint32_t(1) << shift) - 1);  // işaret genişletme
        dst[i] = T(int32_t(result));
    }
    return pos == buf.size();
}

}  // namespace

bool loaded() { return net != nullptr; }
const std::string& description() { return netDescription; }

bool load(const std::string& path, std::string* err) {
    auto fail = [&](const std::string& msg) { if (err) *err = msg; return false; };
    static const bool tablesOk = init_tables();
    if (!tablesOk) return fail("tehdit tabloları tutarsız");

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("dosya açılamadı: " + path);

    auto n = std::make_unique<Network>();
    if (read_u32(in) != Version) return fail("beklenmeyen NNUE sürümü (Stockfish 19 ağı gerekli)");
    read_u32(in);  // mimari karması
    uint32_t descSize = read_u32(in);
    std::string desc(descSize, '\0');
    in.read(desc.data(), descSize);
    read_u32(in);  // ilk katman karması

    int8_t* threatW = n->otherWeight;
    int8_t* pairW = n->otherWeight + size_t(ThreatDims) * HalfDims;
    int32_t* threatP = n->otherPsqt;
    int32_t* pairP = n->otherPsqt + size_t(ThreatDims) * PsqtBuckets;
    if (!read_leb128(in, n->ftBias, HalfDims) || !read_array(in, threatW, size_t(ThreatDims) * HalfDims)
        || !read_leb128(in, threatP, size_t(ThreatDims) * PsqtBuckets)
        || !read_array(in, pairW, size_t(PairDims) * HalfDims)
        || !read_leb128(in, pairP, size_t(PairDims) * PsqtBuckets)
        || !read_leb128(in, n->psqWeight, size_t(PsqDims) * HalfDims)
        || !read_leb128(in, n->psqPsqt, size_t(PsqDims) * PsqtBuckets))
        return fail("ağ dosyası eksik ya da bozuk (ilk katman)");

    for (auto& ls : n->stacks) {
        read_u32(in);  // yığın karması
        if (!read_array(in, ls.b0, L2) || !read_array(in, ls.w0, L2 * HalfDims) || !read_array(in, ls.b1, L3)
            || !read_array(in, ls.w1, L3 * L2 * 2) || !read_array(in, ls.b2, 1)
            || !read_array(in, ls.w2, L2 * 2 + L3 * 2))
            return fail("ağ dosyası eksik (katman yığını)");
        for (int c = 0; c < HalfDims / 4; ++c)
            for (int o = 0; o < L2; ++o)
                for (int j = 0; j < 4; ++j) ls.w0T[(c * L2 + o) * 4 + j] = ls.w0[o * HalfDims + c * 4 + j];
    }
    in.peek();
    if (!in.eof()) return fail("dosyanın sonunda fazladan veri var");

    net = std::move(n);
    netDescription = desc;
    return true;
}

// ---------------- Sıfırdan değerlendirme ----------------
Output evaluate_scratch(const Position& pos) {
    alignas(64) int16_t acc[COLOR_NB][HalfDims];
    int32_t psqt[COLOR_NB][PsqtBuckets];
    auto ts = std::make_unique<ThreatState>();
    collect_threats(pos, *ts);
    auto list = std::make_unique<IndexList>();
    IndexList none;
    for (Color p : {WHITE, BLACK}) {
        std::memcpy(acc[p], net->ftBias, sizeof(acc[p]));
        std::memset(psqt[p], 0, sizeof(psqt[p]));
        FeatureCtx ctx(p, pos.king_square(p));
        for (Bitboard b = pos.pieces(); b;) {
            Square s = pop_lsb(b);
            int idx = ctx.index(s, pos.piece_on(s));
            add_row(acc[p], net->psqWeight + size_t(idx) * HalfDims);
            add_psqt(psqt[p], net->psqPsqt + size_t(idx) * PsqtBuckets);
        }
        list->n = 0;
        active_other(p, pos.king_square(p), *ts, *list);
        apply_other(acc[p], psqt[p], *list, none);
    }
    const Color us = pos.side_to_move();
    const int bucket = (popcount(pos.pieces()) - 1) / 4;
    return output(acc[us], Zero16, acc[~us], Zero16, psqt[us][bucket], psqt[~us][bucket], bucket);
}

// ---------------- Artımlı değerlendirici ----------------
Evaluator::Evaluator() { clear(); }

void Evaluator::clear() {
    stack.assign(256, Accumulator{});
    threats.assign(256, ThreatState{});
    finny.assign(size_t(COLOR_NB) * SQUARE_NB, FinnyEntry{});
    if (net)
        for (auto& e : finny) std::memcpy(e.acc, net->ftBias, sizeof(e.acc));  // boş tahta = sadece bias
}

void Evaluator::ensure_threats(const Position& pos, int idx) {
    if (threats[idx].key != pos.key()) collect_threats(pos, threats[idx]);
}

// Finny tablosundan HalfKA kısmı: kayıtlı dizilimle şimdiki arasındaki farkı uygula
void Evaluator::refresh_psq(const Position& pos, int idx, Color persp) {
    ++refreshes;
    const Square ksq = pos.king_square(persp);
    FinnyEntry& e = finny[int(persp) * SQUARE_NB + int(ksq)];
    FeatureCtx ctx(persp, ksq);

    RowDelta d;
    const int32_t* addP[64];
    const int32_t* subP[64];
    for (Color c : {WHITE, BLACK})
        for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) {
            const Bitboard oldBB = e.byColor[c] & e.byType[pt];
            const Bitboard newBB = pos.pieces(c, pt);
            const Piece pc = make_piece(c, pt);
            for (Bitboard b = oldBB & ~newBB; b;) {
                const int i = ctx.index(pop_lsb(b), pc);
                if (d.nSub16 == 64) break;
                subP[d.nSub16] = net->psqPsqt + size_t(i) * PsqtBuckets;
                d.sub16[d.nSub16++] = net->psqWeight + size_t(i) * HalfDims;
            }
            for (Bitboard b = newBB & ~oldBB; b;) {
                const int i = ctx.index(pop_lsb(b), pc);
                if (d.nAdd16 == 64) break;
                addP[d.nAdd16] = net->psqPsqt + size_t(i) * PsqtBuckets;
                d.add16[d.nAdd16++] = net->psqWeight + size_t(i) * HalfDims;
            }
        }
    // En fazla 32 taş çıkar + 32 taş girer, 64'lük diziler yeter
    apply_delta(e.acc, e.acc, d);
    apply_delta_psqt(e.psqt, e.psqt, d, addP, subP);
    for (Color c : {WHITE, BLACK}) e.byColor[c] = pos.pieces(c);
    for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) e.byType[pt] = pos.pieces(pt);

    Accumulator& acc = stack[idx];
    std::memcpy(acc.psq[persp], e.acc, sizeof(e.acc));
    std::memcpy(acc.psqtPsq[persp], e.psqt, sizeof(e.psqt));
}

// Tembel güncelleme: geriye doğru hesaplanmış en yakın ataya git.
//  - HalfKA: yolda bu bakışın şahı oynadıysa (kova/ayna değişir) Finny tablosundan
//    tazele, yoksa yol üzerindeki taş değişimlerini uygula.
//  - Tehdit + piyon çifti: sadece yöne (şahın tahtanın hangi yarısında olduğu)
//    bağlı. Yön aynıysa ata ile şimdiki pozisyonun listelerinin farkını uygula,
//    değiştiyse (şah ortayı geçti) etkin listeden sıfırdan topla.
void Evaluator::update(const Position& pos, int idx, Color persp) {
    auto valid = [&](int j) {
        const Key k = pos.state_at(j).key;
        return stack[j].key[persp] == k && threats[j].key == k;
    };
    if (valid(idx)) return;

    constexpr int MaxSpan = 16;  // daha uzak ata varsa baştan kurmak daha ucuz
    const Square ksq = pos.king_square(persp);
    Accumulator& out = stack[idx];
    int j = idx;
    bool kingMoved = false;
    while (!valid(j)) {
        if (j == 0 || idx - j >= MaxSpan) {
            // Kullanılabilir ata yok: ikisini de baştan
            refresh_psq(pos, idx, persp);
            RowDelta d;
            active_other(persp, ksq, threats[idx], d.add8);
            rows += d.add8.n;
            apply_delta(out.thr[persp], Zero16, d);
            const int32_t zero[PsqtBuckets] = {};
            apply_delta_psqt(out.psqtThr[persp], zero, d, nullptr, nullptr);
            out.key[persp] = pos.state_at(idx).key;
            out.ksq[persp] = ksq;
            return;
        }
        const DirtyPiece& dp = pos.state_at(j).dirty;
        if (dp.count && dp.piece[0] == make_piece(persp, KING)) kingMoved = true;
        --j;
    }
    const Accumulator& in = stack[j];

    // HalfKA kısmı
    if (kingMoved) refresh_psq(pos, idx, persp);
    else {
        ++updates;
        FeatureCtx ctx(persp, ksq);  // şah bu yolda oynamadı, kare aynı
        RowDelta d;
        const int32_t* addP[64];
        const int32_t* subP[64];
        for (int k = j + 1; k <= idx; ++k) {
            const DirtyPiece& dp = pos.state_at(k).dirty;
            for (int i = 0; i < dp.count; ++i) {
                if (dp.from[i] != SQ_NONE) {
                    const int f = ctx.index(dp.from[i], dp.piece[i]);
                    __builtin_prefetch(net->psqWeight + size_t(f) * HalfDims);
                    subP[d.nSub16] = net->psqPsqt + size_t(f) * PsqtBuckets;
                    d.sub16[d.nSub16++] = net->psqWeight + size_t(f) * HalfDims;
                }
                if (dp.to[i] != SQ_NONE) {
                    const int t = ctx.index(dp.to[i], dp.piece[i]);
                    __builtin_prefetch(net->psqWeight + size_t(t) * HalfDims);
                    addP[d.nAdd16] = net->psqPsqt + size_t(t) * PsqtBuckets;
                    d.add16[d.nAdd16++] = net->psqWeight + size_t(t) * HalfDims;
                }
            }
        }
        apply_delta(out.psq[persp], in.psq[persp], d);
        apply_delta_psqt(out.psqtPsq[persp], in.psqtPsq[persp], d, addP, subP);
    }

    // Tehdit + piyon çifti kısmı
    RowDelta d;
    if (other_orient(persp, in.ksq[persp]) == other_orient(persp, ksq)) {
        other_changes(persp, ksq, threats[j], threats[idx], d.add8, d.sub8);
        rows += d.add8.n + d.sub8.n;
        apply_delta(out.thr[persp], in.thr[persp], d);
        apply_delta_psqt(out.psqtThr[persp], in.psqtThr[persp], d, nullptr, nullptr);
    } else {
        active_other(persp, ksq, threats[idx], d.add8);
        rows += d.add8.n;
        apply_delta(out.thr[persp], Zero16, d);
        const int32_t zero[PsqtBuckets] = {};
        apply_delta_psqt(out.psqtThr[persp], zero, d, nullptr, nullptr);
    }
    out.key[persp] = pos.state_at(idx).key;
    out.ksq[persp] = ksq;
}

Output Evaluator::evaluate(const Position& pos) {
    const int idx = pos.state_index();
    if (int(stack.size()) <= idx) {
        stack.resize(idx + 256);
        threats.resize(idx + 256);
    }
    ensure_threats(pos, idx);
    update(pos, idx, WHITE);
    update(pos, idx, BLACK);
    const Accumulator& a = stack[idx];
    const Color us = pos.side_to_move(), them = ~us;
    const int bucket = (popcount(pos.pieces()) - 1) / 4;
    return output(a.psq[us], a.thr[us], a.psq[them], a.thr[them], a.psqtPsq[us][bucket] + a.psqtThr[us][bucket],
                  a.psqtPsq[them][bucket] + a.psqtThr[them][bucket], bucket);
}

}  // namespace mai::NNUE
