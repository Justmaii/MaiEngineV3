using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace MaiAraclar;

/// <summary>
/// UCI motor maçı: gerçek saat, paralel oyunlar, süreden kaybetme tespiti.
///
///     dotnet run --project tools/MaiAraclar -c Release -- mac --a ./maiengine --b ../opp/inphish5 \
///         --tc 10+0.1 --games 100 --concurrency 2 --openings tools/openings2.epd --out maclar/v3-vs-inphish5
///
/// Her açılış iki kez oynanır (renkler değişir). Saat, motorun cevabı beklenirken
/// geçen gerçek süreyle düşülür (GUI'deki gibi); saat eksiye düşerse süreden kayıp.
/// Adjudication (isteğe bağlı): iki motor da 5 hamle boyunca >= 1000 cp derse
/// kazanç; 40. hamleden sonra 8 hamle boyunca |skor| &lt;= 10 cp ise beraberlik.
/// </summary>
public static class Mac
{
    private sealed record Ayar(string A, string B, string AAd, string BAd,
        Dictionary<string, string> AOpt, Dictionary<string, string> BOpt,
        double Taban, double Artis, string Tc, string Etkinlik, bool Hakem);

    private sealed record OyunSonucu(int No, double? PuanA, string Sonuc, string Neden, string Pgn, bool KaybedenA);

    public static int Calistir(Secenekler s)
    {
        string tc = s.Tek("tc") ?? "10+0.1";
        var tcParca = tc.Split('+');
        var ayar = new Ayar(
            s.Tek("a") ?? throw new ArgumentException("--a gerekli"),
            s.Tek("b") ?? throw new ArgumentException("--b gerekli"),
            s.Tek("a-name") ?? "A", s.Tek("b-name") ?? "B",
            SecenekSozlugu(s.Hepsi("a-opt")), SecenekSozlugu(s.Hepsi("b-opt")),
            double.Parse(tcParca[0], CultureInfo.InvariantCulture),
            tcParca.Length > 1 ? double.Parse(tcParca[1], CultureInfo.InvariantCulture) : 0,
            tc, s.Tek("event") ?? "MaiEngine v3 test", s.Var("adjudicate"));

        int oyunSayisi = int.Parse(s.Tek("games") ?? "100");
        int paralel = int.Parse(s.Tek("concurrency") ?? "2");
        int baslangic = int.Parse(s.Tek("start") ?? "0");
        string cikti = s.Tek("out") ?? "maclar/mac";
        string acilisDosyasi = s.Tek("openings") ?? Path.Combine("tools", "openings2.epd");

        var fenler = File.ReadLines(acilisDosyasi).Where(l => l.Trim().Length > 0)
            .Select(l => l.Split(';')[0].Trim())
            .Select(f => f.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length == 4 ? f + " 0 1" : f)
            .ToList();
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(cikti))!);

        int g = 0, b = 0, m = 0, sureA = 0, sureB = 0, biten = 0;
        var kilit = new object();
        var saat = Stopwatch.StartNew();
        using var pgnDosya = new StreamWriter(cikti + ".pgn", append: true);
        using var logDosya = new StreamWriter(cikti + ".log", append: true);

        Parallel.For(0, oyunSayisi, new ParallelOptions { MaxDegreeOfParallelism = paralel }, i =>
        {
            string fen = fenler[(baslangic + i / 2) % fenler.Count];
            OyunSonucu r;
            try { r = OyunOyna(ayar, fen, i % 2 == 0, i + 1); }
            catch (Exception e) { r = new OyunSonucu(i + 1, null, "*", "crash:" + e.Message, "", false); }

            lock (kilit)
            {
                if (r.PuanA is not double puan)
                {
                    Console.WriteLine($"oyun {r.No}: ÇÖKTÜ {r.Neden}");
                    return;
                }
                if (puan == 1) g++; else if (puan == 0) m++; else b++;
                if (r.Neden == "time_forfeit") { if (r.KaybedenA) sureA++; else sureB++; }
                pgnDosya.Write(r.Pgn + "\n\n");
                pgnDosya.Flush();
                var (elo, hata) = EloHesapla(g, b, m);
                int n = g + b + m;
                biten = n;
                string satir = $"[{n}/{oyunSayisi}] oyun {r.No}: {r.Sonuc} ({r.Neden})  " +
                    $"{ayar.AAd}: +{g} ={b} -{m}  {(100.0 * (g + 0.5 * b) / n).ToString("0.0", CultureInfo.InvariantCulture)}%  " +
                    $"elo {elo.ToString("+0;-0;+0", CultureInfo.InvariantCulture)} ±{hata.ToString("0", CultureInfo.InvariantCulture)}  " +
                    $"süreden kayıp {ayar.AAd}:{sureA} {ayar.BAd}:{sureB}  ({saat.Elapsed.TotalSeconds:0}s)";
                Console.WriteLine(satir);
                logDosya.WriteLine(satir);
                logDosya.Flush();
            }
        });
        return 0;
    }

    private static Dictionary<string, string> SecenekSozlugu(IEnumerable<string> liste) =>
        liste.Select(o => o.Split('=', 2)).Where(p => p.Length == 2).ToDictionary(p => p[0], p => p[1]);

    private static OyunSonucu OyunOyna(Ayar ayar, string fen, bool aBeyaz, int oyunNo)
    {
        using var A = new UciMotor(ayar.A, ayar.AOpt);
        using var B = new UciMotor(ayar.B, ayar.BOpt);
        A.YeniOyun();
        B.YeniOyun();

        var tahta = new Tahta(fen);
        var hamleler = new List<(Hamle hamle, string san, int? cp, int? derinlik, double sure)>();
        var tekrar = new Dictionary<string, int> { [tahta.KonumAnahtari()] = 1 };
        double beyazSaat = ayar.Taban, siyahSaat = ayar.Taban;
        int[] kazanSeri = new int[2], kaybetSeri = new int[2];
        int beraberSeri = 0, ply = 0;
        string sonuc, neden;

        while (true)
        {
            var bitis = OyunSonu(tahta, tekrar);
            if (bitis != null) { (sonuc, neden) = bitis.Value; break; }
            if (ply >= 600) { sonuc = "1/2-1/2"; neden = "max_ply"; break; }

            bool beyaz = tahta.BeyazMi;
            int taraf = beyaz ? 0 : 1;
            var motor = beyaz == aBeyaz ? A : B;
            string kaybeden = beyaz ? "0-1" : "1-0";

            string pozisyon = "position fen " + fen +
                (hamleler.Count > 0 ? " moves " + string.Join(' ', hamleler.Select(h => h.hamle.Uci)) : "");
            string go = $"go wtime {Ms(beyazSaat)} btime {Ms(siyahSaat)} winc {Ms(ayar.Artis)} binc {Ms(ayar.Artis)}";

            var t0 = Stopwatch.StartNew();
            AramaSonucu r;
            try { r = motor.Ara(pozisyon, go); }
            catch (Exception e) { sonuc = kaybeden; neden = "engine_error:" + e.Message; break; }
            double harcanan = t0.Elapsed.TotalSeconds;

            if (beyaz) beyazSaat -= harcanan; else siyahSaat -= harcanan;
            if ((beyaz ? beyazSaat : siyahSaat) < 0) { sonuc = kaybeden; neden = "time_forfeit"; break; }
            if (beyaz) beyazSaat += ayar.Artis; else siyahSaat += ayar.Artis;

            var hamle = tahta.UciOku(r.EnIyi);
            if (hamle.BosMu || !tahta.YasalMi(hamle)) { sonuc = kaybeden; neden = "illegal_move:" + r.EnIyi; break; }

            int? cp = r.Sayi;
            hamleler.Add((hamle, tahta.San(hamle), cp, r.Derinlik, harcanan));
            tahta.Uygula(hamle);
            ply++;
            string anahtar = tahta.KonumAnahtari();
            tekrar[anahtar] = tekrar.GetValueOrDefault(anahtar) + 1;

            if (ayar.Hakem && cp is int c)
            {
                // Kazanç: kazanan kendi hamlelerinde >= +1000, kaybeden kendi hamlelerinde <= -1000 (5'er kez üst üste)
                kazanSeri[taraf] = c >= 1000 ? kazanSeri[taraf] + 1 : 0;
                kaybetSeri[taraf] = c <= -1000 ? kaybetSeri[taraf] + 1 : 0;
                if (kazanSeri[taraf] >= 5 && kaybetSeri[1 - taraf] >= 5)
                {
                    sonuc = beyaz ? "1-0" : "0-1"; neden = "adjudicated_win"; break;
                }
                beraberSeri = Math.Abs(c) <= 10 && tahta.HamleNo > 40 ? beraberSeri + 1 : 0;
                if (beraberSeri >= 16) { sonuc = "1/2-1/2"; neden = "adjudicated_draw"; break; }
            }
        }

        string pgn = PgnYaz(ayar, fen, aBeyaz, oyunNo, sonuc, neden, hamleler);
        double puanA = sonuc == "1/2-1/2" ? 0.5 : ((sonuc == "1-0") == aBeyaz ? 1.0 : 0.0);
        return new OyunSonucu(oyunNo, puanA, sonuc, neden, pgn, puanA == 0.0);
    }

    private static int Ms(double saniye) => (int)Math.Max(1, Math.Round(saniye * 1000));

    /// <summary>python-chess'in outcome(claim_draw=True) kuralları.</summary>
    private static (string sonuc, string neden)? OyunSonu(Tahta t, Dictionary<string, int> tekrar)
    {
        bool hamleVar = t.YasalHamleler().Count > 0;
        if (!hamleVar && t.SahCekilmis()) return (t.BeyazMi ? "0-1" : "1-0", "checkmate");
        if (t.YetersizMalzeme()) return ("1/2-1/2", "insufficient_material");
        if (!hamleVar) return ("1/2-1/2", "stalemate");
        if (t.YariHamle >= 150) return ("1/2-1/2", "seventyfive_moves");
        int sayi = tekrar.GetValueOrDefault(t.KonumAnahtari());
        if (sayi >= 5) return ("1/2-1/2", "fivefold_repetition");
        if (t.YariHamle >= 100) return ("1/2-1/2", "fifty_moves");
        if (sayi >= 3) return ("1/2-1/2", "threefold_repetition");
        return null;
    }

    private static string PgnYaz(Ayar ayar, string fen, bool aBeyaz, int oyunNo, string sonuc, string neden,
                                 List<(Hamle hamle, string san, int? cp, int? derinlik, double sure)> hamleler)
    {
        var sb = new StringBuilder();
        void Baslik(string ad, string deger) => sb.Append('[').Append(ad).Append(" \"").Append(deger.Replace("\"", "\\\"")).Append("\"]\n");
        Baslik("Event", ayar.Etkinlik);
        Baslik("Site", "?");
        Baslik("Date", "????.??.??");
        Baslik("Round", oyunNo.ToString());
        Baslik("White", aBeyaz ? ayar.AAd : ayar.BAd);
        Baslik("Black", aBeyaz ? ayar.BAd : ayar.AAd);
        Baslik("Result", sonuc);
        Baslik("FEN", fen);
        Baslik("SetUp", "1");
        Baslik("TimeControl", ayar.Tc);
        Baslik("Termination", neden);
        sb.Append('\n');

        // Hamle metni, python-chess gibi 80 sütunda kırılır
        var parcalar = new List<string>();
        var t = new Tahta(fen);
        bool yorumSonrasi = true;
        foreach (var (hamle, san, cp, derinlik, sure) in hamleler)
        {
            if (t.BeyazMi) parcalar.Add($"{t.HamleNo}.");
            else if (yorumSonrasi) parcalar.Add($"{t.HamleNo}...");
            parcalar.Add(san);
            string yorum = cp is int c
                ? $"{PyFloat(c / 100.0)}/{(derinlik?.ToString() ?? "None")} {sure.ToString("0.000", CultureInfo.InvariantCulture)}s"
                : $"{sure.ToString("0.000", CultureInfo.InvariantCulture)}s";
            parcalar.Add("{ " + yorum + " }");
            yorumSonrasi = true;
            t.Uygula(hamle);
        }
        parcalar.Add(sonuc);

        int sutun = 0;
        foreach (var p in parcalar)
        {
            if (sutun > 0 && sutun + 1 + p.Length > 80) { sb.Append('\n'); sutun = 0; }
            else if (sutun > 0) { sb.Append(' '); sutun++; }
            sb.Append(p);
            sutun += p.Length;
        }
        return sb.ToString();
    }

    /// <summary>Python'un f"{x:+}" biçimi: +0.56, +0.0, -0.4, +999.93</summary>
    private static string PyFloat(double x)
    {
        string s = x.ToString("R", CultureInfo.InvariantCulture);
        if (!s.Contains('.') && !s.Contains('E')) s += ".0";
        return x >= 0 && !s.StartsWith('-') ? "+" + s : s;
    }

    private static (double elo, double hata) EloHesapla(int g, int b, int m)
    {
        int n = g + b + m;
        if (n == 0) return (0, 0);
        double s = (g + 0.5 * b) / n;
        double varyans = (g * Math.Pow(1 - s, 2) + b * Math.Pow(0.5 - s, 2) + m * Math.Pow(0 - s, 2)) / n;
        double se = n > 1 ? Math.Sqrt(varyans / n) : 0.5;
        static double Elo(double p)
        {
            p = Math.Clamp(p, 1e-3, 1 - 1e-3);
            return -400 * Math.Log10(1 / p - 1);
        }
        double e = Elo(s), alt = Elo(s - 1.96 * se), ust = Elo(s + 1.96 * se);
        return (e, (ust - alt) / 2);
    }
}
