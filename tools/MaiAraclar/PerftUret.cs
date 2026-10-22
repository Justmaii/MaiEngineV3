namespace MaiAraclar;

/// <summary>
/// Rastgele pozisyonlar üretip perft sayılarını hesaplar (EPD: "fen ;D1 n ;D2 n ;D3 n").
///
///     dotnet run --project tools/MaiAraclar -c Release -- perft-uret 600 3 > tools/perft_random.epd
///     ./maiengine epd tools/perft_random.epd
///
/// Rastgele oyunlar terfi, en passant, rok ve şah durumlarını bol bol içersin diye
/// hamle seçimi bunlara ağırlık verir. Ayrıca bilinen tuzak pozisyonlar eklenir.
/// Not: depodaki perft_random.epd python-chess ile üretilmişti (bağımsız kütüphane);
/// bu araç aynı biçimde yeni pozisyonlar üretir.
/// </summary>
public static class PerftUret
{
    private static readonly string[] Tuzaklar =
    {
        // En passant yatay açmaz: alırsan iki piyon birden kalkar, şah açılır
        "8/8/8/K2pP2r/8/8/8/7k w - d6 0 1",
        "8/8/8/KPp4r/8/8/8/7k w - c6 0 1",
        "7k/8/8/r2pP2K/8/8/8/8 w - d6 0 1",
        // En passant ile şahtan kurtulma / çapraz açmaz
        "8/8/8/2k5/3Pp3/8/8/4K3 b - d3 0 1",
        "8/8/8/8/k2Pp2Q/8/8/3K4 b - d3 0 1",
        "8/5bk1/8/2Pp4/8/1K6/8/8 w - d6 0 1",
        "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1",
        // Rok: geçilen kare tehdit altında / hat açık
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "r3k2r/8/8/8/8/8/8/1R2K2R b Kkq - 0 1",
        "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1",
        "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1",
        "4k3/8/8/8/8/8/8/R3K2r w Q - 0 1",
        // Terfi / alışlı terfi / şah
        "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
        "8/P1k5/K7/8/8/8/8/8 w - - 0 1",
        "2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1",
        // Çifte şah
        "4k3/8/8/8/8/8/4q3/3RK2r w - - 0 1",
        "8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1",
        // Pat / mat
        "k7/8/1Q6/8/8/8/8/7K b - - 0 1",
        "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
        "K1k5/8/P7/8/8/8/8/8 w - - 0 1",
    };

    public static int Calistir(string[] args)
    {
        int adet = args.Length > 0 ? int.Parse(args[0]) : 500;
        int derinlik = args.Length > 1 ? int.Parse(args[1]) : 3;
        var rng = new Random(20260927);
        foreach (var fen in Tuzaklar.Concat(RastgelePozisyonlar(adet, rng)))
        {
            var t = new Tahta(fen);
            var parca = new List<string> { fen };
            for (int d = 1; d <= derinlik; d++) parca.Add($"D{d} {Perft(t, d)}");
            Console.WriteLine(string.Join(" ;", parca));
        }
        return 0;
    }

    private static long Perft(Tahta t, int d)
    {
        var hamleler = t.YasalHamleler();
        if (d == 1) return hamleler.Count;
        long n = 0;
        foreach (var h in hamleler)
        {
            var k = t.Kopya();
            k.Uygula(h);
            n += Perft(k, d - 1);
        }
        return n;
    }

    private static List<string> RastgelePozisyonlar(int adet, Random rng)
    {
        var cikti = new List<string>();
        var gorulen = new HashSet<string>();
        void Ekle(string fen) { if (gorulen.Add(fen)) cikti.Add(fen); }

        while (cikti.Count < adet)
        {
            var b = new Tahta();
            int ply = rng.Next(4, 141);
            for (int i = 0; i < ply; i++)
            {
                var hamleler = b.YasalHamleler();
                if (hamleler.Count == 0) break;
                var ozel = hamleler.Where(m => m.Terfi != 0 || b.GecerkenMi(m) || b.RokMu(m) || SahVerir(b, m)).ToList();
                var alislar = hamleler.Where(b.AlisMi).ToList();
                double r = rng.NextDouble();
                Hamle secilen = ozel.Count > 0 && r < 0.35 ? ozel[rng.Next(ozel.Count)]
                              : alislar.Count > 0 && r < 0.6 ? alislar[rng.Next(alislar.Count)]
                              : hamleler[rng.Next(hamleler.Count)];
                b.Uygula(secilen);
                // Arada bir yolda kalan pozisyonları da kaydet (en passant fırsatları için)
                if (b.GecerkenAlinabilir() && rng.NextDouble() < 0.5) Ekle(b.Fen());
            }
            if (b.YasalHamleler().Count > 0 && !b.YetersizMalzeme()) Ekle(b.Fen());
        }
        return cikti.Take(adet).ToList();
    }

    private static bool SahVerir(Tahta t, Hamle h)
    {
        var k = t.Kopya();
        k.Uygula(h);
        return k.SahCekilmis();
    }
}
