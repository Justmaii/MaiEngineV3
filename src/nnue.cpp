// MaiEngine v3 — NNUE değerlendirmesi (Stockfish 19 ağı)
// Copyright (C) 2026 Mai (Justmaii). GPLv3, bkz. LICENSE.
#include "nnue.h"

#include <algorithm>
#include <array>
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
    bool hasPsqt;  // SF19 ağında var; SFNNv17 ağında (nn-252f33942263) psqt çıkışı yok
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

inline uint32_t threat_index(uint32_t raw, int orient, int swap) {
    const int a = int(raw & 15) ^ swap, d = int((raw >> 4) & 15) ^ swap;
    const int f = int((raw >> 8) & 63) ^ orient, t = int((raw >> 14) & 63) ^ orient;
    return ThreatLut1[a][d][f < t] + ThreatOffsets[a][f] + ThreatLut2[a][f][t];
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
    uint32_t v[2 * MaxThreats + 1024];  // tüm etkin tehditler + piyon çiftleri sığar
    int n = 0;
    void push(uint32_t i) {
        __builtin_prefetch(net->otherWeight + size_t(i) * HalfDims);
        v[n++] = i;
    }
};

void active_other(Color persp, Square ksq, const uint32_t* threats, int n, const Bitboard* pawns, IndexList& out) {
    const int orient = other_orient(persp, ksq), swap = 8 * persp;
    for (int i = 0; i < n; ++i) {
        const uint32_t idx = threat_index(threats[i], orient, swap);
        if (idx < uint32_t(ThreatDims)) out.push(idx);
    }
    const Bitboard white = pawns[WHITE], black = pawns[BLACK];
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

void active_other(Color persp, Square ksq, const Position& pos, IndexList& out) {
    uint32_t list[2 * MaxThreats];
    const int n = pos.all_threats(list, 2 * MaxThreats);
    const Bitboard pawns[COLOR_NB] = {pos.pieces(WHITE, PAWN), pos.pieces(BLACK, PAWN)};
    active_other(persp, ksq, list, n, pawns, out);
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

// k'ya giden hamlenin tehdit değişikliklerini bu bakışın indekslerine çevir; piyon
// çiftleri iki durumun piyonlarının farkından. Aynı hamlede kalkıp yeniden eklenen
// nadir tehditler ayıklanmaz (iki satır fazla, sıralamaktan ucuz). Hamlenin listesi
// taşmışsa false (çağıran baştan hesaplar).
bool other_changes(const Position& pos, int k, Color persp, Square ksq, IndexList& added, IndexList& removed) {
    const int orient = other_orient(persp, ksq), swap = 8 * persp;
    const ThreatDelta& d = pos.threat_delta(k);
    if (d.overflow) return false;
    for (int i = 0; i < d.count; ++i) {
        const uint32_t idx = threat_index(d.raw[i], orient, swap);
        if (idx < uint32_t(ThreatDims)) ((d.raw[i] & ThreatAdded) ? added : removed).push(idx);
    }

    const ThreatDelta& a = pos.threat_delta(k - 1);
    const Bitboard bw = a.pawns[WHITE], bb = a.pawns[BLACK], aw = d.pawns[WHITE], ab = d.pawns[BLACK];
    if (bw != aw || bb != ab) {
        pair_changes(persp, orient, aw & ~bw, ab & ~bb, aw, ab, added);
        pair_changes(persp, orient, bw & ~aw, bb & ~ab, bw, bb, removed);
    }
    return true;
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

// Bir güncellemede eklenip çıkarılacak satırlar: int16 satırlar (HalfKA ağırlıkları ya da
// bütün bir accumulator / Finny kaydı) ve int8 satırlar (tehdit / piyon çifti).
// Her int16 satırın psqt karşılığı da yanında.
struct RowDelta {
    static constexpr int Max16 = 72;
    const int16_t* add16[Max16];
    const int16_t* sub16[Max16];
    const int32_t* addP[Max16];
    const int32_t* subP[Max16];
    int nAdd16 = 0, nSub16 = 0;
    IndexList add8, sub8;

    void add_raw(const int16_t* r, const int32_t* p) { add16[nAdd16] = r, addP[nAdd16++] = p; }
    void sub_raw(const int16_t* r, const int32_t* p) { sub16[nSub16] = r, subP[nSub16++] = p; }
    void add_psq(int i) { add_raw(net->psqWeight + size_t(i) * HalfDims, net->psqPsqt + size_t(i) * PsqtBuckets); }
    void sub_psq(int i) { sub_raw(net->psqWeight + size_t(i) * HalfDims, net->psqPsqt + size_t(i) * PsqtBuckets); }
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

void apply_delta_psqt(int32_t* out, const int32_t* in, const RowDelta& d) {  // out == in olabilir
    if (!net->hasPsqt) return;  // hep sıfır kalır
    int32_t p[PsqtBuckets];
    std::memcpy(p, in, sizeof(p));
    for (int i = 0; i < d.nAdd16; ++i) add_psqt(p, d.addP[i]);
    for (int i = 0; i < d.nSub16; ++i) sub_psqt(p, d.subP[i]);
    for (int i = 0; i < d.add8.n; ++i) add_psqt(p, net->otherPsqt + size_t(d.add8.v[i]) * PsqtBuckets);
    for (int i = 0; i < d.sub8.n; ++i) sub_psqt(p, net->otherPsqt + size_t(d.sub8.v[i]) * PsqtBuckets);
    std::memcpy(out, p, sizeof(p));
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
inline void transform_half(const int16_t* __restrict p, uint8_t* __restrict out) {
    constexpr int H = HalfDims / 2;
#if defined(__ARM_NEON)
    const int16x8_t zero = vdupq_n_s16(0), max = vdupq_n_s16(255);
    auto clip = [&](int i) { return vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(vld1q_s16(p + i), zero), max)); };
    for (int j = 0; j < H; j += 16) {
        // 255 * 255 = 65025 uint16'ya sığar
        const uint16x8_t a0 = clip(j), a1 = clip(j + 8), b0 = clip(H + j), b1 = clip(H + j + 8);
        vst1q_u8(out + j, vcombine_u8(vmovn_u16(vshrq_n_u16(vmulq_u16(a0, b0), 9)), vmovn_u16(vshrq_n_u16(vmulq_u16(a1, b1), 9))));
    }
    return;
#endif
    for (int j = 0; j < H; ++j) {
        unsigned a = unsigned(std::clamp<int>(p[j], 0, 255));
        unsigned b = unsigned(std::clamp<int>(p[j + H], 0, 255));
        out[j] = uint8_t((a * b) / 512);
    }
}

inline uint8_t sqr_clipped(int32_t x, int shift) { return uint8_t(std::min<int64_t>(127, (int64_t(x) * x) >> shift)); }
inline uint8_t clipped(int32_t x, int shift) { return uint8_t(std::clamp(x >> shift, 0, 127)); }

int propagate(const uint8_t* input, int bucket) {
    const LayerStack& ls = net->stacks[bucket];

    // Girdinin çoğu sıfır: sadece sıfır olmayan 4 baytlık parçaların ağırlık sütunlarını ekle
    alignas(64) int32_t fc0[L2];
    uint16_t nnz[HalfDims / 4 + 8];
    int nnzCount = 0;
    uint32_t in32[HalfDims / 4];
    std::memcpy(in32, input, HalfDims);
#if defined(__ARM_NEON)
    // 8'li gruplar: sıfır olmayanların bit maskesi -> hazır indeks tablosundan tek yazma
    static const auto NnzLut = [] {
        std::array<std::array<uint16_t, 8>, 256> t{};
        for (int m = 0; m < 256; ++m)
            for (int b = 0, k = 0; b < 8; ++b)
                if (m & (1 << b)) t[m][k++] = uint16_t(b);
        return t;
    }();
    const uint16x8_t bitVals = {1, 2, 4, 8, 16, 32, 64, 128};
    for (int c = 0; c < HalfDims / 4; c += 8) {
        const uint32x4_t a = vld1q_u32(in32 + c), b = vld1q_u32(in32 + c + 4);
        const uint16x8_t nz = vcombine_u16(vmovn_u32(vtstq_u32(a, a)), vmovn_u32(vtstq_u32(b, b)));
        const unsigned mask = vaddvq_u16(vandq_u16(nz, bitVals));
        vst1q_u16(nnz + nnzCount, vaddq_u16(vld1q_u16(NnzLut[mask].data()), vdupq_n_u16(uint16_t(c))));
        nnzCount += popcount(mask);
    }
#else
    for (int c = 0; c < HalfDims / 4; ++c) {  // dalsız
        nnz[nnzCount] = uint16_t(c);
        nnzCount += in32[c] != 0;
    }
#endif
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


Output output(const int16_t* us, const int16_t* them, int32_t psqtUs, int32_t psqtThem, int bucket) {
    alignas(64) uint8_t input[HalfDims];
    transform_half(us, input);
    transform_half(them, input + HalfDims / 2);
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
bool has_psqt() { return net->hasPsqt; }
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
    if (!read_leb128(in, n->ftBias, HalfDims) || !read_array(in, threatW, size_t(ThreatDims) * HalfDims))
        return fail("ağ dosyası eksik ya da bozuk (ilk katman)");

    // Tehdit ağırlıklarından sonra sıkıştırılmış blok geliyorsa psqt'li (SF19) ağ, ham
    // piyon çifti ağırlıkları geliyorsa psqt'siz (SFNNv17) ağ
    char next[17];
    const auto mark = in.tellg();
    in.read(next, sizeof(next));
    n->hasPsqt = in && std::memcmp(next, "COMPRESSED_LEB128", sizeof(next)) == 0;
    in.seekg(mark);
    bool ok;
    if (n->hasPsqt)
        ok = read_leb128(in, threatP, size_t(ThreatDims) * PsqtBuckets)
          && read_array(in, pairW, size_t(PairDims) * HalfDims)
          && read_leb128(in, pairP, size_t(PairDims) * PsqtBuckets)
          && read_leb128(in, n->psqWeight, size_t(PsqDims) * HalfDims)
          && read_leb128(in, n->psqPsqt, size_t(PsqDims) * PsqtBuckets);
    else {
        ok = read_array(in, pairW, size_t(PairDims) * HalfDims)
          && read_leb128(in, n->psqWeight, size_t(PsqDims) * HalfDims);
        std::memset(n->psqPsqt, 0, sizeof(n->psqPsqt));
        std::memset(n->otherPsqt, 0, sizeof(n->otherPsqt));
    }
    if (!ok) return fail("ağ dosyası eksik ya da bozuk (ilk katman)");

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
        active_other(p, pos.king_square(p), pos, *list);
        apply_other(acc[p], psqt[p], *list, none);
    }
    const Color us = pos.side_to_move();
    const int bucket = (popcount(pos.pieces()) - 1) / 4;
    return output(acc[us], acc[~us], psqt[us][bucket], psqt[~us][bucket], bucket);
}

// ---------------- Artımlı değerlendirici ----------------
Evaluator::Evaluator() { clear(); }

void Evaluator::clear() {
    stack.assign(256, Accumulator{});
    finny.assign(size_t(COLOR_NB) * SQUARE_NB, FinnyEntry{});
    if (net)
        for (auto& e : finny) std::memcpy(e.acc, net->ftBias, sizeof(e.acc));  // boş tahta = sadece bias
}

namespace {

// Finny kaydının dizilimiyle verilen dizilim arasındaki HalfKA farkı. invert: işaretleri
// ters çevir (kayıttan o dizilime değil, o dizilimden kayda gitmek için).
void finny_diff(const FinnyEntry& e, const Bitboard* byColor, const Bitboard* byType, const FeatureCtx& ctx,
                RowDelta& d, bool invert) {
    for (Color c : {WHITE, BLACK})
        for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) {
            const Bitboard oldBB = e.byColor[c] & e.byType[pt], newBB = byColor[c] & byType[pt];
            const Piece pc = make_piece(c, pt);
            for (Bitboard b = oldBB & ~newBB; b;) {
                const int i = ctx.index(pop_lsb(b), pc);
                invert ? d.add_psq(i) : d.sub_psq(i);
            }
            for (Bitboard b = newBB & ~oldBB; b;) {
                const int i = ctx.index(pop_lsb(b), pc);
                invert ? d.sub_psq(i) : d.add_psq(i);
            }
        }
}

// Finny kaydını verilen dizilime getir
void finny_update(FinnyEntry& e, const Bitboard* byColor, const Bitboard* byType, const FeatureCtx& ctx) {
    RowDelta d;
    finny_diff(e, byColor, byType, ctx, d, false);
    apply_delta(e.acc, e.acc, d);
    apply_delta_psqt(e.psqt, e.psqt, d);
    for (Color c : {WHITE, BLACK}) e.byColor[c] = byColor[c];
    for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) e.byType[pt] = byType[pt];
}

// Bir hamlenin HalfKA farkı (forward: hamleyi uygula, değilse geri al)
void dirty_diff(const DirtyPiece& dp, const FeatureCtx& ctx, RowDelta& d, bool forward) {
    for (int i = 0; i < dp.count; ++i) {
        if (dp.from[i] != SQ_NONE) {
            const int f = ctx.index(dp.from[i], dp.piece[i]);
            forward ? d.sub_psq(f) : d.add_psq(f);
        }
        if (dp.to[i] != SQ_NONE) {
            const int t = ctx.index(dp.to[i], dp.piece[i]);
            forward ? d.add_psq(t) : d.sub_psq(t);
        }
    }
}

}  // namespace

// HalfKA kısmı Finny tablosundan, tehdit + piyon çifti şimdiki tahtadan sıfırdan
void Evaluator::refresh(const Position& pos, int idx, Color persp) {
    ++refreshes;
    const Square ksq = pos.king_square(persp);
    FinnyEntry& e = finny[int(persp) * SQUARE_NB + int(ksq)];
    Bitboard byColor[COLOR_NB], byType[PIECE_TYPE_NB];
    for (Color c : {WHITE, BLACK}) byColor[c] = pos.pieces(c);
    for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) byType[pt] = pos.pieces(pt);
    finny_update(e, byColor, byType, FeatureCtx(persp, ksq));

    RowDelta t;
    active_other(persp, ksq, pos, t.add8);
    rows += t.add8.n;
    Accumulator& acc = stack[idx];
    apply_delta(acc.acc[persp], e.acc, t);
    apply_delta_psqt(acc.psqt[persp], e.psqt, t);
    acc.key[persp] = pos.state_at(idx).key;
}

// Tek hamlelik fark: k-1 hesaplı, k'yı hesapla (aradaki hamlede bu bakışın şahı oynamadı)
bool Evaluator::forward(const Position& pos, int k, Color persp) {
    const Square ksq = pos.threat_delta(k).ksq[persp];
    RowDelta d;
    if (!other_changes(pos, k, persp, ksq, d.add8, d.sub8)) return false;
    dirty_diff(pos.state_at(k).dirty, FeatureCtx(persp, ksq), d, true);
    ++updates;
    rows += d.add8.n + d.sub8.n;
    apply_delta(stack[k].acc[persp], stack[k - 1].acc[persp], d);
    apply_delta_psqt(stack[k].psqt[persp], stack[k - 1].psqt[persp], d);
    stack[k].key[persp] = pos.state_at(k).key;
    return true;
}

// Geriye doğru: k+1 hesaplı, k'yı hesapla (k+1'e giden hamlede bu bakışın şahı oynamadı).
// Tazelemeden sonra yoldaki durumları doldurur; kardeş düğümler oradan başlar.
bool Evaluator::backward(const Position& pos, int k, Color persp) {
    const Square ksq = pos.threat_delta(k + 1).ksq[persp];
    RowDelta d;
    if (!other_changes(pos, k + 1, persp, ksq, d.sub8, d.add8)) return false;
    dirty_diff(pos.state_at(k + 1).dirty, FeatureCtx(persp, ksq), d, false);
    ++updates;
    rows += d.add8.n + d.sub8.n;
    apply_delta(stack[k].acc[persp], stack[k + 1].acc[persp], d);
    apply_delta_psqt(stack[k].psqt[persp], stack[k + 1].psqt[persp], d);
    stack[k].key[persp] = pos.state_at(k).key;
    return true;
}

// Son hamlede bu bakışın şahı tahtanın aynı yarısında oynadı (Stockfish 19'daki "hybrid"):
// tehditlerin yönü değişmez. Yeni = yeni şahın Finny kaydı + önceki accumulator
//   - eski şahın Finny kaydı (önceki dizilime getirilmiş hali), artı tehdit farkı.
bool Evaluator::hybrid(const Position& pos, int idx, Color persp) {
    const DirtyPiece& dp = pos.state_at(idx).dirty;
    const Square oldKsq = dp.from[0], newKsq = dp.to[0];

    RowDelta d;
    if (!other_changes(pos, idx, persp, newKsq, d.add8, d.sub8)) return false;
    ++refreshes;

    Bitboard byColor[COLOR_NB], byType[PIECE_TYPE_NB];
    for (Color c : {WHITE, BLACK}) byColor[c] = pos.pieces(c);
    for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) byType[pt] = pos.pieces(pt);
    FinnyEntry& newEntry = finny[int(persp) * SQUARE_NB + int(newKsq)];
    finny_update(newEntry, byColor, byType, FeatureCtx(persp, newKsq));

    // Önceki dizilim: şah geri, alınan taş yerine
    const Bitboard kingMove = square_bb(oldKsq) | square_bb(newKsq);
    byColor[persp] ^= kingMove;
    byType[KING] ^= kingMove;
    if (dp.count == 2) {
        byColor[color_of(dp.piece[1])] |= square_bb(dp.from[1]);
        byType[type_of(dp.piece[1])] |= square_bb(dp.from[1]);
    }
    const FinnyEntry& oldEntry = finny[int(persp) * SQUARE_NB + int(oldKsq)];
    d.add_raw(stack[idx - 1].acc[persp], stack[idx - 1].psqt[persp]);
    d.sub_raw(oldEntry.acc, oldEntry.psqt);
    finny_diff(oldEntry, byColor, byType, FeatureCtx(persp, oldKsq), d, true);

    rows += d.add8.n + d.sub8.n;
    apply_delta(stack[idx].acc[persp], newEntry.acc, d);
    apply_delta_psqt(stack[idx].psqt[persp], newEntry.psqt, d);
    stack[idx].key[persp] = pos.state_at(idx).key;
    return true;
}

// Tembel güncelleme (Stockfish 19 gibi): geriye doğru ilk hesaplı durumu ya da bu bakışın
// şahının oynadığı hamleyi bul. Hesaplı durum bulunduysa oradan ileri her hamleyi tek tek
// hesaplayıp sakla. Şah oynadıysa: son hamleyse ve aynı yarıdaysa hybrid, değilse şimdiki
// pozisyonu tazele ve aradaki durumları geriye doğru doldur.
void Evaluator::update(const Position& pos, int idx, Color persp) {
    auto valid = [&](int j) { return stack[j].key[persp] == pos.state_at(j).key; };
    if (valid(idx)) return;
    const Piece ourKing = make_piece(persp, KING);
    auto kingMoved = [&](int j) {
        const DirtyPiece& dp = pos.state_at(j).dirty;
        return dp.count > 0 && dp.piece[0] == ourKing;
    };

    int j = idx;
    while (!valid(j) && j > 0 && !kingMoved(j)) --j;

    if (valid(j)) {
        for (int k = j + 1; k <= idx; ++k)
            if (!forward(pos, k, persp)) {  // fark listesi taştı (çok nadir)
                refresh(pos, idx, persp);
                for (int b = idx - 1; b >= k && backward(pos, b, persp); --b) {}
                return;
            }
        return;
    }

    if (j == idx && idx > 0 && valid(idx - 1) && popcount(pos.pieces()) >= 15) {
        const DirtyPiece& dp = pos.state_at(idx).dirty;
        const bool castling = dp.count == 2 && dp.to[1] != SQ_NONE;
        if (!castling && (int(dp.from[0]) & 4) == (int(dp.to[0]) & 4) && hybrid(pos, idx, persp)) return;
    }

    refresh(pos, idx, persp);
    for (int b = idx - 1; b >= j && backward(pos, b, persp); --b) {}
}

Output Evaluator::evaluate(const Position& pos) {
    ++evals;
    const int idx = pos.state_index();
    if (int(stack.size()) <= idx) stack.resize(idx + 256);
    update(pos, idx, WHITE);
    update(pos, idx, BLACK);
    const Accumulator& a = stack[idx];
    const Color us = pos.side_to_move(), them = ~us;
    const int bucket = (popcount(pos.pieces()) - 1) / 4;
    return output(a.acc[us], a.acc[them], a.psqt[us][bucket], a.psqt[them][bucket], bucket);
}

}  // namespace mai::NNUE
