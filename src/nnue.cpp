// MaiEngine v3 — NNUE değerlendirmesi (Stockfish 15.1 ağı, HalfKAv2_hm)
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

constexpr uint32_t Version = 0x7AF32F20u;
constexpr int WeightScaleBits = 6;
constexpr int OutputScale = 16;

// ---------------- Ağırlıklar ----------------
struct LayerStack {
    alignas(64) int32_t b0[L1Out];
    alignas(64) int8_t w0[L1Out * HalfDims];
    alignas(64) int32_t b1[L2Out];
    alignas(64) int8_t w1[L2Out * L2In];
    alignas(64) int32_t b2[1];
    alignas(64) int8_t w2[L3In];
};

struct Network {
    alignas(64) int16_t ftBias[HalfDims];
    alignas(64) int16_t ftWeight[InputDims * HalfDims];  // 46 MB
    alignas(64) int32_t psqtWeight[InputDims * PsqtBuckets];
    LayerStack stacks[LayerStacks];
};

std::unique_ptr<Network> net;
std::string netDescription;

// ---------------- Özellik indeksi ----------------
// Şah vezir kanadındaysa tahta yatay aynalanır; siyah bakışında ayrıca dikey.
inline int orient(Color persp, Square ksq) {
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
    FeatureCtx(Color p, Square ksq) : orient(NNUE::orient(p, ksq)), bucketBase(king_bucket_base(p, ksq)), persp(p) {}
    int index(Square s, Piece pc) const { return (int(s) ^ orient) + piece_base(persp, pc) + bucketBase; }
};

// ---------------- Vektör yardımcıları ----------------
// Tüm döngüler sabit uzunlukta; derleyici -O3 ile vektörleştiriyor.
inline void add_row(int16_t* __restrict acc, const int16_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] += w[i];
}
inline void sub_row(int16_t* __restrict acc, const int16_t* __restrict w) {
    for (int i = 0; i < HalfDims; ++i) acc[i] -= w[i];
}
// Tek geçişte kopyala + ekle/çıkar (en sık durum: 1 ekle 1 çıkar, alışta 1 ekle 2 çıkar)
inline void copy_add_sub(int16_t* __restrict out, const int16_t* __restrict in,
                         const int16_t* const* adds, int nAdd, const int16_t* const* subs, int nSub) {
    if (nAdd == 1 && nSub == 1) {
        const int16_t *a = adds[0], *s = subs[0];
        for (int i = 0; i < HalfDims; ++i) out[i] = in[i] + a[i] - s[i];
    } else if (nAdd == 1 && nSub == 2) {
        const int16_t *a = adds[0], *s1 = subs[0], *s2 = subs[1];
        for (int i = 0; i < HalfDims; ++i) out[i] = in[i] + a[i] - s1[i] - s2[i];
    } else if (nAdd == 2 && nSub == 2) {
        const int16_t *a1 = adds[0], *a2 = adds[1], *s1 = subs[0], *s2 = subs[1];
        for (int i = 0; i < HalfDims; ++i) out[i] = in[i] + a1[i] + a2[i] - s1[i] - s2[i];
    } else {
        std::memcpy(out, in, sizeof(int16_t) * HalfDims);
        for (int k = 0; k < nAdd; ++k) add_row(out, adds[k]);
        for (int k = 0; k < nSub; ++k) sub_row(out, subs[k]);
    }
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

// İkişerli çarpım: 1024 toplam -> 512 bayt (her bakış için)
inline void transform_half(const int16_t* __restrict acc, uint8_t* __restrict out) {
    constexpr int H = HalfDims / 2;
    for (int j = 0; j < H; ++j) {
        int a = std::clamp<int>(acc[j], 0, 127);
        int b = std::clamp<int>(acc[j + H], 0, 127);
        out[j] = uint8_t((a * b) >> 7);
    }
}

int propagate(const uint8_t* input, int bucket) {
    const LayerStack& ls = net->stacks[bucket];

    int32_t fc0[L1Out];
    for (int o = 0; o < L1Out; ++o) fc0[o] = ls.b0[o] + dot_u8i8(input, ls.w0 + o * HalfDims, HalfDims);

    // Kare alınmış ve kırpılmış iki aktivasyon yan yana
    alignas(64) uint8_t h1[L2In] = {};
    for (int i = 0; i < L1Out - 1; ++i) {
        int64_t sq = (int64_t(fc0[i]) * fc0[i]) >> (2 * WeightScaleBits + 7);
        h1[i] = uint8_t(std::min<int64_t>(sq, 127));
        h1[L1Out - 1 + i] = uint8_t(std::clamp(fc0[i] >> WeightScaleBits, 0, 127));
    }

    alignas(64) uint8_t h2[L3In];
    for (int o = 0; o < L2Out; ++o) {
        int32_t s = ls.b1[o] + dot_u8i8(h1, ls.w1 + o * L2In, L2In);
        h2[o] = uint8_t(std::clamp(s >> WeightScaleBits, 0, 127));
    }

    int32_t out = ls.b2[0] + dot_u8i8(h2, ls.w2, L3In);
    // 16. çıkış ağı atlayıp doğrudan sonuca eklenir
    int32_t fwd = fc0[L1Out - 1] * (600 * OutputScale) / (127 * (1 << WeightScaleBits));
    return out + fwd;
}

int output(const int16_t* us, const int16_t* them, int32_t psqtUs, int32_t psqtThem, int bucket) {
    alignas(64) uint8_t input[HalfDims];
    transform_half(us, input);
    transform_half(them, input + HalfDims / 2);
    int psqt = (psqtUs - psqtThem) / 2;
    return (psqt + propagate(input, bucket)) / OutputScale;
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

}  // namespace

bool loaded() { return net != nullptr; }
const std::string& description() { return netDescription; }

bool load(const std::string& path, std::string* err) {
    auto fail = [&](const std::string& msg) { if (err) *err = msg; return false; };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("dosya açılamadı: " + path);

    auto n = std::make_unique<Network>();
    if (read_u32(in) != Version) return fail("beklenmeyen NNUE sürümü (SF15.1 HalfKAv2_hm ağı gerekli)");
    read_u32(in);  // mimari karması
    uint32_t descSize = read_u32(in);
    std::string desc(descSize, '\0');
    in.read(desc.data(), descSize);
    read_u32(in);  // ilk katman karması

    if (!read_array(in, n->ftBias, HalfDims) || !read_array(in, n->ftWeight, size_t(InputDims) * HalfDims)
        || !read_array(in, n->psqtWeight, size_t(InputDims) * PsqtBuckets))
        return fail("ağ dosyası eksik (ilk katman)");

    for (auto& ls : n->stacks) {
        read_u32(in);  // yığın karması
        if (!read_array(in, ls.b0, L1Out) || !read_array(in, ls.w0, L1Out * HalfDims)
            || !read_array(in, ls.b1, L2Out) || !read_array(in, ls.w1, L2Out * L2In)
            || !read_array(in, ls.b2, 1) || !read_array(in, ls.w2, L3In))
            return fail("ağ dosyası eksik (katman yığını)");
    }
    in.peek();
    if (!in.eof()) return fail("dosyanın sonunda fazladan veri var");

    net = std::move(n);
    netDescription = desc;
    return true;
}

// ---------------- Sıfırdan değerlendirme ----------------
int evaluate_scratch(const Position& pos) {
    alignas(64) int16_t acc[COLOR_NB][HalfDims];
    int32_t psqt[COLOR_NB][PsqtBuckets];
    for (Color p : {WHITE, BLACK}) {
        std::memcpy(acc[p], net->ftBias, sizeof(acc[p]));
        std::memset(psqt[p], 0, sizeof(psqt[p]));
        FeatureCtx ctx(p, pos.king_square(p));
        for (Bitboard b = pos.pieces(); b;) {
            Square s = pop_lsb(b);
            int idx = ctx.index(s, pos.piece_on(s));
            add_row(acc[p], net->ftWeight + size_t(idx) * HalfDims);
            for (int k = 0; k < PsqtBuckets; ++k) psqt[p][k] += net->psqtWeight[idx * PsqtBuckets + k];
        }
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

// Finny tablosundan: kayıtlı dizilimle şimdiki arasındaki farkı uygula
void Evaluator::refresh(const Position& pos, Accumulator& acc, Color persp) {
    ++refreshes;
    const Square ksq = pos.king_square(persp);
    FinnyEntry& e = finny[int(persp) * SQUARE_NB + int(ksq)];
    FeatureCtx ctx(persp, ksq);

    for (Color c : {WHITE, BLACK})
        for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) {
            const Bitboard oldBB = e.byColor[c] & e.byType[pt];
            const Bitboard newBB = pos.pieces(c, pt);
            const Piece pc = make_piece(c, pt);
            for (Bitboard b = oldBB & ~newBB; b;) {
                int idx = ctx.index(pop_lsb(b), pc);
                sub_row(e.acc, net->ftWeight + size_t(idx) * HalfDims);
                for (int k = 0; k < PsqtBuckets; ++k) e.psqt[k] -= net->psqtWeight[idx * PsqtBuckets + k];
            }
            for (Bitboard b = newBB & ~oldBB; b;) {
                int idx = ctx.index(pop_lsb(b), pc);
                add_row(e.acc, net->ftWeight + size_t(idx) * HalfDims);
                for (int k = 0; k < PsqtBuckets; ++k) e.psqt[k] += net->psqtWeight[idx * PsqtBuckets + k];
            }
        }
    for (Color c : {WHITE, BLACK}) e.byColor[c] = pos.pieces(c);
    for (PieceType pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) e.byType[pt] = pos.pieces(pt);

    std::memcpy(acc.acc[persp], e.acc, sizeof(e.acc));
    std::memcpy(acc.psqt[persp], e.psqt, sizeof(e.psqt));
}

// Tembel güncelleme: geriye doğru hesaplanmış en yakın ataya git, oradan ileri
// sadece değişen taşları uygula. Yol üzerinde bu bakışın şahı oynadıysa
// (kova/ayna değişir) Finny tablosundan tazele.
void Evaluator::update(const Position& pos, int idx, Color persp) {
    auto valid = [&](int j) { return stack[j].key[persp] == pos.state_at(j).key; };
    if (valid(idx)) return;

    int j = idx;
    bool needRefresh = false;
    while (!valid(j)) {
        const DirtyPiece& dp = pos.state_at(j).dirty;
        if (j == 0 || (dp.count && dp.piece[0] == make_piece(persp, KING))) {
            needRefresh = true;
            break;
        }
        --j;
    }

    if (needRefresh) {
        refresh(pos, stack[idx], persp);
        stack[idx].key[persp] = pos.state_at(idx).key;
        return;
    }

    FeatureCtx ctx(persp, pos.king_square(persp));  // şah bu yolda oynamadı, kare aynı
    for (int k = j + 1; k <= idx; ++k) {
        ++updates;
        const DirtyPiece& dp = pos.state_at(k).dirty;
        const int16_t* adds[3];
        const int16_t* subs[3];
        int nAdd = 0, nSub = 0;
        Accumulator& out = stack[k];
        const Accumulator& in = stack[k - 1];
        std::memcpy(out.psqt[persp], in.psqt[persp], sizeof(out.psqt[persp]));
        for (int i = 0; i < dp.count; ++i) {
            if (dp.from[i] != SQ_NONE) {
                int f = ctx.index(dp.from[i], dp.piece[i]);
                subs[nSub++] = net->ftWeight + size_t(f) * HalfDims;
                for (int q = 0; q < PsqtBuckets; ++q) out.psqt[persp][q] -= net->psqtWeight[f * PsqtBuckets + q];
            }
            if (dp.to[i] != SQ_NONE) {
                int t = ctx.index(dp.to[i], dp.piece[i]);
                adds[nAdd++] = net->ftWeight + size_t(t) * HalfDims;
                for (int q = 0; q < PsqtBuckets; ++q) out.psqt[persp][q] += net->psqtWeight[t * PsqtBuckets + q];
            }
        }
        copy_add_sub(out.acc[persp], in.acc[persp], adds, nAdd, subs, nSub);
        out.key[persp] = pos.state_at(k).key;
    }
}

int Evaluator::evaluate(const Position& pos) {
    const int idx = pos.state_index();
    if (int(stack.size()) <= idx) stack.resize(idx + 256);
    update(pos, idx, WHITE);
    update(pos, idx, BLACK);
    const Accumulator& a = stack[idx];
    const Color us = pos.side_to_move();
    const int bucket = (popcount(pos.pieces()) - 1) / 4;
    return output(a.acc[us], a.acc[~us], a.psqt[us][bucket], a.psqt[~us][bucket], bucket);
}

}  // namespace mai::NNUE
