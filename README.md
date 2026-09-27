# MaiEngine v3

C++20 ile sıfırdan yazılan satranç motoru. MaiEngine v2'nin (C#) devamı; hedef:
**inphish**'i yenmek.

Lisans: **GPLv3** (bkz. `LICENSE`). Değerlendirme için Stockfish 15.1 NNUE ağı
kullanılacak; arama Stockfish 15.1'deki formüller/parametreler örnek alınarak
yazılacak (kod kopyalanmadan). Kaynak: <https://github.com/official-stockfish/Stockfish>.

## Durum

| Adım | Durum |
|---|---|
| 1. Bitboard tahta, legal hamle üretimi (pin maskesi), perft | ✅ |
| 2. NNUE (SF15.1 HalfKAv2_hm), AVX2 + NEON, Finny tablosu | ⏳ |
| 3. Arama (SF15.1 tarzı), kovalı TT, history'ler, SEE (şah kuralıyla) | ⏳ |
| 4. UCI, Lazy SMP, zaman yönetimi, Syzygy | ⏳ |
| 5. Maç testleri (openings2.epd, SPRT) | ⏳ |

## Derleme

```sh
make            # optimize (-O3, -march/-mcpu=native, LTO)
make debug      # assert + AddressSanitizer + UBSan -> maiengine-debug
make test       # perft takımı + python-chess'e karşı 1020 pozisyon + tutarlılık
```

## Komutlar

```
maiengine perft <d> [fen]        toplam düğüm
maiengine divide <d> [fen]       kök hamle başına düğüm (Stockfish "go perft" biçimi)
maiengine suite [full]           bilinen perft sayıları
maiengine epd <dosya> [maxD]     EPD perft takımı ("FEN ;D1 n ;D2 n ...")
maiengine verify <d> [fen]       her düğümde: bitboard/board tutarlılığı, Zobrist,
                                 piyon anahtarı, checkers, CAPTURES+QUIETS=LEGAL,
                                 FEN gidiş-dönüş, undo, null move
maiengine verifyepd <dosya> [d]  EPD'deki her pozisyonda verify
maiengine bench                  perft hızı
```

## Tasarım (adım 1)

- **Tahta:** `board[64]` + tip/renk bitboard'ları, `StateInfo` yığını (anahtar,
  piyon anahtarı, rok hakları, en passant, 50 hamle, alınan taş, şah çekenler).
- **Saldırılar:** fancy magic bitboard; magic sayıları açılışta sabit tohumla
  aranır (birkaç ms).
- **Hamle kodu:** 16 bit, Stockfish ile aynı yerleşim (hedef, kaynak, terfi, tip).
- **Legal üretim:** önce "oyna-kontrol et" yok, yasallık maskelerle sağlanıyor:
  - `danger`: rakibin saldırdığı kareler, bizim şah tahtadan kaldırılmış olarak
    (şah, kalenin hattında geri kaçabilir sanılmasın diye)
  - `checkmask`: tek şahta şah çeken taş + aradaki kareler; çifte şahta sadece şah oynar
  - `pinHV` / `pinD`: açmazdaki taşlar sadece kendi hattında gidebilir
  - en passant tahtada denenir (yatay açmaz tuzağı: `K..Pp..r`)
  - rok: geçilen kareler `danger`'a bakılır
- **Üretim tipleri:** `CAPTURES` (tüm alışlar + vezire itiş-terfi), `QUIETS`
  (geri kalan), `LEGAL` (hepsi). `verify` ikisinin birleşiminin tam olarak
  `LEGAL` olduğunu kontrol eder — arama bunlara güvenecek.
- **En passant karesi** sadece gerçekten alabilecek rakip piyon varsa anahtara
  girer (aynı pozisyon = aynı anahtar, TT için önemli).

## Doğrulama

| Test | Sonuç |
|---|---|
| chessprogramming.org 7 pozisyon, en derin (805M düğüm) | hepsi birebir |
| python-chess'e karşı 1020 pozisyon (20 tuzak + 1000 rastgele), D1–D3 | 0 hata |
| `verifyepd` sanitizer'lı sürümle, aynı 1020 pozisyon, D2 (338 bin düğüm) | 0 hata |

## Hız (perft, toplu sayım, tek çekirdek)

| Makine | `bench` |
|---|---|
| Bulut VM (Xeon 2.1 GHz) | ~210 Mnps |
| MacBook Air, Linux VM (aarch64, g++ 11) | ~278 Mnps |

(v2'de C# ile bu ölçülmemişti; asıl kıyas arama hızında olacak: v2 200-370k nps,
inphish ~1,24M nps.)

## Negatif sonuçlar

_(Denenen ama kazanç getirmeyen değişiklikler buraya yazılır.)_
