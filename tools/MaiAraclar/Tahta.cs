using System.Text;

namespace MaiAraclar;

/// <summary>Taş türleri. Taş = tür * renk (beyaz +1, siyah -1); 0 boş kare.</summary>
public static class Tas
{
    public const int Bos = 0, Piyon = 1, At = 2, Fil = 3, Kale = 4, Vezir = 5, Sah = 6;

    public static int Tur(int tas) => Math.Abs(tas);
    public static int Renk(int tas) => Math.Sign(tas);

    /// <summary>Malzeme değeri (şah 0) — kurban tespiti için.</summary>
    public static int Deger(int tur) => tur switch
    {
        Piyon => 1, At => 3, Fil => 3, Kale => 5, Vezir => 9, _ => 0
    };

    public static char Harf(int tas)
    {
        char c = " pnbrqk"[Tur(tas)];
        return tas > 0 ? char.ToUpperInvariant(c) : c;
    }

    public static int HarftenTas(char c)
    {
        int tur = "pnbrqk".IndexOf(char.ToLowerInvariant(c)) + 1;
        if (tur == 0) throw new FormatException($"Geçersiz taş harfi: {c}");
        return char.IsUpper(c) ? tur : -tur;
    }
}

/// <summary>Kare indeksi: a1 = 0, h1 = 7, a8 = 56, h8 = 63.</summary>
public static class Kare
{
    public static int Sutun(int k) => k & 7;
    public static int Sira(int k) => k >> 3;
    public static int Yap(int sutun, int sira) => sira * 8 + sutun;
    public static string Ad(int k) => $"{(char)('a' + Sutun(k))}{Sira(k) + 1}";

    public static int Oku(string s)
    {
        if (s.Length != 2 || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8')
            throw new FormatException($"Geçersiz kare: {s}");
        return Yap(s[0] - 'a', s[1] - '1');
    }
}

public readonly record struct Hamle(int Kimden, int Kime, int Terfi = 0)
{
    public static readonly Hamle Yok = new(-1, -1);
    public bool BosMu => Kimden < 0;

    public string Uci => BosMu ? "0000"
        : Kare.Ad(Kimden) + Kare.Ad(Kime) + (Terfi != 0 ? " pnbrqk"[Terfi].ToString() : "");

    public override string ToString() => Uci;
}

/// <summary>
/// Sade, okunaklı bir satranç tahtası: 64 karelik dizi, sözde-yasal hamle üretip
/// şahı açıkta bırakanları eleyen yasal hamle üretimi, FEN ve SAN.
/// Hız önemli değil (analizde motor çalışıyor), doğruluk önemli.
/// </summary>
public sealed class Tahta
{
    public const string BaslangicFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    // Rok hakları
    public const int BeyazKisa = 1, BeyazUzun = 2, SiyahKisa = 4, SiyahUzun = 8;

    private readonly int[] _kare = new int[64];
    public int Sira { get; private set; } = 1;          // +1 beyaz, -1 siyah
    public int Rok { get; private set; }
    public int GecerkenKare { get; private set; } = -1;
    public int YariHamle { get; private set; }
    public int HamleNo { get; private set; } = 1;

    public int this[int kare] => _kare[kare];
    public bool BeyazMi => Sira == 1;

    private static readonly (int ds, int dr)[] AtAdimlari =
        { (1, 2), (2, 1), (2, -1), (1, -2), (-1, -2), (-2, -1), (-2, 1), (-1, 2) };
    private static readonly (int ds, int dr)[] SahAdimlari =
        { (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1) };
    private static readonly (int ds, int dr)[] FilYonleri = { (1, 1), (-1, 1), (1, -1), (-1, -1) };
    private static readonly (int ds, int dr)[] KaleYonleri = { (1, 0), (-1, 0), (0, 1), (0, -1) };

    public Tahta() : this(BaslangicFen) { }

    public Tahta(string fen)
    {
        var parca = fen.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parca.Length < 4) throw new FormatException($"Geçersiz FEN: {fen}");

        var siralar = parca[0].Split('/');
        if (siralar.Length != 8) throw new FormatException($"Geçersiz FEN: {fen}");
        for (int i = 0; i < 8; i++)
        {
            int sira = 7 - i, sutun = 0;
            foreach (char c in siralar[i])
            {
                if (char.IsDigit(c)) sutun += c - '0';
                else
                {
                    if (sutun > 7) throw new FormatException($"Geçersiz FEN: {fen}");
                    _kare[Kare.Yap(sutun++, sira)] = Tas.HarftenTas(c);
                }
            }
            if (sutun != 8) throw new FormatException($"Geçersiz FEN: {fen}");
        }

        Sira = parca[1] == "b" ? -1 : 1;
        foreach (char c in parca[2])
            Rok |= c switch { 'K' => BeyazKisa, 'Q' => BeyazUzun, 'k' => SiyahKisa, 'q' => SiyahUzun, _ => 0 };
        GecerkenKare = parca[3] == "-" ? -1 : Kare.Oku(parca[3]);
        if (parca.Length > 4 && int.TryParse(parca[4], out int y)) YariHamle = y;
        if (parca.Length > 5 && int.TryParse(parca[5], out int n)) HamleNo = Math.Max(1, n);
    }

    private Tahta(Tahta t)
    {
        Array.Copy(t._kare, _kare, 64);
        Sira = t.Sira; Rok = t.Rok; GecerkenKare = t.GecerkenKare;
        YariHamle = t.YariHamle; HamleNo = t.HamleNo;
    }

    public Tahta Kopya() => new(this);

    // ---------------------------------------------------------------- FEN

    public string Fen()
    {
        var sb = new StringBuilder();
        for (int sira = 7; sira >= 0; sira--)
        {
            int bos = 0;
            for (int sutun = 0; sutun < 8; sutun++)
            {
                int t = _kare[Kare.Yap(sutun, sira)];
                if (t == 0) { bos++; continue; }
                if (bos > 0) { sb.Append(bos); bos = 0; }
                sb.Append(Tas.Harf(t));
            }
            if (bos > 0) sb.Append(bos);
            if (sira > 0) sb.Append('/');
        }
        sb.Append(BeyazMi ? " w " : " b ");
        string rok = (Rok & BeyazKisa) != 0 ? "K" : "";
        if ((Rok & BeyazUzun) != 0) rok += "Q";
        if ((Rok & SiyahKisa) != 0) rok += "k";
        if ((Rok & SiyahUzun) != 0) rok += "q";
        sb.Append(rok.Length > 0 ? rok : "-");
        // python-chess gibi: geçerken kareyi yalnızca gerçekten alınabiliyorsa yaz
        sb.Append(' ').Append(GecerkenAlinabilir() ? Kare.Ad(GecerkenKare) : "-");
        sb.Append(' ').Append(YariHamle).Append(' ').Append(HamleNo);
        return sb.ToString();
    }

    /// <summary>Tekrar tespiti için: taşlar, sıra, rok, geçerken.</summary>
    public string KonumAnahtari()
    {
        var f = Fen().Split(' ');
        return string.Join(' ', f[0], f[1], f[2], f[3]);
    }

    // ---------------------------------------------------------------- saldırılar

    private static bool Icinde(int sutun, int sira) => sutun >= 0 && sutun < 8 && sira >= 0 && sira < 8;

    /// <summary>Verilen renkten kareye saldıran taşların kareleri.</summary>
    public List<int> Saldiranlar(int renk, int kare)
    {
        var sonuc = new List<int>();
        int s = Kare.Sutun(kare), r = Kare.Sira(kare);

        // Piyon: saldıran piyon, karenin bir sıra "gerisinde" çaprazda durur
        int pr = r - renk;
        foreach (int ds in new[] { -1, 1 })
            if (Icinde(s + ds, pr) && _kare[Kare.Yap(s + ds, pr)] == Tas.Piyon * renk)
                sonuc.Add(Kare.Yap(s + ds, pr));

        foreach (var (ds, dr) in AtAdimlari)
            if (Icinde(s + ds, r + dr) && _kare[Kare.Yap(s + ds, r + dr)] == Tas.At * renk)
                sonuc.Add(Kare.Yap(s + ds, r + dr));

        foreach (var (ds, dr) in SahAdimlari)
            if (Icinde(s + ds, r + dr) && _kare[Kare.Yap(s + ds, r + dr)] == Tas.Sah * renk)
                sonuc.Add(Kare.Yap(s + ds, r + dr));

        Kaydiranlar(FilYonleri, Tas.Fil);
        Kaydiranlar(KaleYonleri, Tas.Kale);
        return sonuc;

        void Kaydiranlar((int ds, int dr)[] yonler, int tur)
        {
            foreach (var (ds, dr) in yonler)
            {
                int x = s + ds, y = r + dr;
                while (Icinde(x, y))
                {
                    int t = _kare[Kare.Yap(x, y)];
                    if (t != 0)
                    {
                        if (Tas.Renk(t) == renk && (Tas.Tur(t) == tur || Tas.Tur(t) == Tas.Vezir))
                            sonuc.Add(Kare.Yap(x, y));
                        break;
                    }
                    x += ds; y += dr;
                }
            }
        }
    }

    public bool SaldiriAltinda(int kare, int saldiranRenk) => Saldiranlar(saldiranRenk, kare).Count > 0;

    public int SahKaresi(int renk) => Array.IndexOf(_kare, Tas.Sah * renk);

    public bool SahCekilmis()
    {
        int k = SahKaresi(Sira);
        return k >= 0 && SaldiriAltinda(k, -Sira);
    }

    // ---------------------------------------------------------------- hamle üretimi

    private IEnumerable<Hamle> SozdeYasal()
    {
        int ben = Sira;
        for (int k = 0; k < 64; k++)
        {
            int t = _kare[k];
            if (t == 0 || Tas.Renk(t) != ben) continue;
            int s = Kare.Sutun(k), r = Kare.Sira(k);

            switch (Tas.Tur(t))
            {
                case Tas.Piyon:
                {
                    int ileri = r + ben;
                    bool terfi = ileri == 7 || ileri == 0;
                    if (Icinde(s, ileri) && _kare[Kare.Yap(s, ileri)] == 0)
                    {
                        foreach (var h in PiyonHamlesi(k, Kare.Yap(s, ileri), terfi)) yield return h;
                        int baslangic = ben == 1 ? 1 : 6;
                        int iki = r + 2 * ben;
                        if (r == baslangic && _kare[Kare.Yap(s, iki)] == 0)
                            yield return new Hamle(k, Kare.Yap(s, iki));
                    }
                    foreach (int ds in new[] { -1, 1 })
                    {
                        if (!Icinde(s + ds, ileri)) continue;
                        int hedef = Kare.Yap(s + ds, ileri);
                        int h = _kare[hedef];
                        if ((h != 0 && Tas.Renk(h) != ben) || hedef == GecerkenKare)
                            foreach (var m in PiyonHamlesi(k, hedef, terfi)) yield return m;
                    }
                    break;
                }
                case Tas.At:
                    foreach (var h in Adimlar(k, AtAdimlari)) yield return h;
                    break;
                case Tas.Sah:
                    foreach (var h in Adimlar(k, SahAdimlari)) yield return h;
                    foreach (var h in RokHamleleri(k)) yield return h;
                    break;
                case Tas.Fil:
                    foreach (var h in Kaymalar(k, FilYonleri)) yield return h;
                    break;
                case Tas.Kale:
                    foreach (var h in Kaymalar(k, KaleYonleri)) yield return h;
                    break;
                case Tas.Vezir:
                    foreach (var h in Kaymalar(k, FilYonleri)) yield return h;
                    foreach (var h in Kaymalar(k, KaleYonleri)) yield return h;
                    break;
            }
        }
    }

    private static IEnumerable<Hamle> PiyonHamlesi(int kimden, int kime, bool terfi)
    {
        if (!terfi) { yield return new Hamle(kimden, kime); yield break; }
        foreach (int tur in new[] { Tas.Vezir, Tas.Kale, Tas.Fil, Tas.At })
            yield return new Hamle(kimden, kime, tur);
    }

    private IEnumerable<Hamle> Adimlar(int k, (int ds, int dr)[] adimlar)
    {
        int s = Kare.Sutun(k), r = Kare.Sira(k);
        foreach (var (ds, dr) in adimlar)
        {
            if (!Icinde(s + ds, r + dr)) continue;
            int hedef = Kare.Yap(s + ds, r + dr);
            if (_kare[hedef] == 0 || Tas.Renk(_kare[hedef]) != Sira) yield return new Hamle(k, hedef);
        }
    }

    private IEnumerable<Hamle> Kaymalar(int k, (int ds, int dr)[] yonler)
    {
        int s = Kare.Sutun(k), r = Kare.Sira(k);
        foreach (var (ds, dr) in yonler)
        {
            int x = s + ds, y = r + dr;
            while (Icinde(x, y))
            {
                int hedef = Kare.Yap(x, y);
                if (_kare[hedef] != 0)
                {
                    if (Tas.Renk(_kare[hedef]) != Sira) yield return new Hamle(k, hedef);
                    break;
                }
                yield return new Hamle(k, hedef);
                x += ds; y += dr;
            }
        }
    }

    private IEnumerable<Hamle> RokHamleleri(int k)
    {
        int ben = Sira;
        int e = ben == 1 ? 4 : 60;
        if (k != e || SaldiriAltinda(e, -ben)) yield break;
        int kisa = ben == 1 ? BeyazKisa : SiyahKisa, uzun = ben == 1 ? BeyazUzun : SiyahUzun;

        if ((Rok & kisa) != 0 && _kare[e + 3] == Tas.Kale * ben
            && _kare[e + 1] == 0 && _kare[e + 2] == 0
            && !SaldiriAltinda(e + 1, -ben) && !SaldiriAltinda(e + 2, -ben))
            yield return new Hamle(e, e + 2);

        if ((Rok & uzun) != 0 && _kare[e - 4] == Tas.Kale * ben
            && _kare[e - 1] == 0 && _kare[e - 2] == 0 && _kare[e - 3] == 0
            && !SaldiriAltinda(e - 1, -ben) && !SaldiriAltinda(e - 2, -ben))
            yield return new Hamle(e, e - 2);
    }

    public List<Hamle> YasalHamleler()
    {
        var sonuc = new List<Hamle>();
        foreach (var h in SozdeYasal())
        {
            var t = Kopya();
            t.Uygula(h);
            int sah = t.SahKaresi(Sira);
            if (sah >= 0 && !t.SaldiriAltinda(sah, -Sira)) sonuc.Add(h);
        }
        return sonuc;
    }

    public bool YasalMi(Hamle h) => YasalHamleler().Contains(h);

    // ---------------------------------------------------------------- hamle yapma

    public bool RokMu(Hamle h) => Tas.Tur(_kare[h.Kimden]) == Tas.Sah && Math.Abs(h.Kime - h.Kimden) == 2;

    public bool GecerkenMi(Hamle h) =>
        Tas.Tur(_kare[h.Kimden]) == Tas.Piyon && h.Kime == GecerkenKare && _kare[h.Kime] == 0
        && Kare.Sutun(h.Kimden) != Kare.Sutun(h.Kime);

    public bool AlisMi(Hamle h) => (_kare[h.Kime] != 0 && !RokMu(h)) || GecerkenMi(h);

    /// <summary>Hamleyi yasallık kontrolü yapmadan uygular.</summary>
    public void Uygula(Hamle h)
    {
        int tas = _kare[h.Kimden], ben = Tas.Renk(tas);
        bool piyon = Tas.Tur(tas) == Tas.Piyon;
        bool alis = AlisMi(h);

        if (GecerkenMi(h)) _kare[h.Kime - 8 * ben] = 0;
        if (RokMu(h))
        {
            bool kisa = h.Kime > h.Kimden;
            int kaleKimden = kisa ? h.Kimden + 3 : h.Kimden - 4;
            int kaleKime = kisa ? h.Kimden + 1 : h.Kimden - 1;
            _kare[kaleKime] = _kare[kaleKimden];
            _kare[kaleKimden] = 0;
        }

        _kare[h.Kime] = h.Terfi != 0 ? h.Terfi * ben : tas;
        _kare[h.Kimden] = 0;

        // Rok hakları: şah ya da kale oynarsa / kale alınırsa
        foreach (int k in new[] { h.Kimden, h.Kime })
            Rok &= k switch
            {
                4 => ~(BeyazKisa | BeyazUzun),
                60 => ~(SiyahKisa | SiyahUzun),
                0 => ~BeyazUzun, 7 => ~BeyazKisa,
                56 => ~SiyahUzun, 63 => ~SiyahKisa,
                _ => ~0
            };

        GecerkenKare = piyon && Math.Abs(h.Kime - h.Kimden) == 16 ? (h.Kimden + h.Kime) / 2 : -1;
        YariHamle = piyon || alis ? 0 : YariHamle + 1;
        if (ben == -1) HamleNo++;
        Sira = -ben;
    }

    /// <summary>Sırayı karşıya verir (null hamle) — "rakip şimdi ne alabilir" sorusu için.</summary>
    public Tahta BosHamle()
    {
        var t = Kopya();
        t.Sira = -Sira;
        t.GecerkenKare = -1;
        return t;
    }

    public bool GecerkenAlinabilir()
    {
        if (GecerkenKare < 0) return false;
        foreach (var h in YasalHamleler())
            if (h.Kime == GecerkenKare && Tas.Tur(_kare[h.Kimden]) == Tas.Piyon) return true;
        return false;
    }

    // ---------------------------------------------------------------- oyun durumu

    public bool MatMi() => SahCekilmis() && YasalHamleler().Count == 0;
    public bool PatMi() => !SahCekilmis() && YasalHamleler().Count == 0;

    public bool YetersizMalzeme()
    {
        int hafif = 0;
        var filRenkleri = new HashSet<int>();
        for (int k = 0; k < 64; k++)
        {
            int tur = Tas.Tur(_kare[k]);
            if (tur is Tas.Piyon or Tas.Kale or Tas.Vezir) return false;
            if (tur is Tas.At or Tas.Fil) hafif++;
            if (tur == Tas.Fil) filRenkleri.Add((Kare.Sutun(k) + Kare.Sira(k)) & 1);
        }
        // K vs K, K + tek hafif taş vs K, ya da yalnızca aynı renk karelerde filler
        if (hafif <= 1) return true;
        bool atVar = _kare.Any(t => Tas.Tur(t) == Tas.At);
        return !atVar && filRenkleri.Count == 1;
    }

    // ---------------------------------------------------------------- SAN

    public string San(Hamle h)
    {
        string san = SanEksiz(h);
        var t = Kopya();
        t.Uygula(h);
        if (t.SahCekilmis()) san += t.YasalHamleler().Count == 0 ? "#" : "+";
        return san;
    }

    private string SanEksiz(Hamle h)
    {
        if (RokMu(h)) return h.Kime > h.Kimden ? "O-O" : "O-O-O";

        int tas = _kare[h.Kimden], tur = Tas.Tur(tas);
        bool alis = AlisMi(h);
        var sb = new StringBuilder();

        if (tur == Tas.Piyon)
        {
            if (alis) sb.Append((char)('a' + Kare.Sutun(h.Kimden))).Append('x');
            sb.Append(Kare.Ad(h.Kime));
            if (h.Terfi != 0) sb.Append('=').Append(char.ToUpperInvariant(" pnbrqk"[h.Terfi]));
            return sb.ToString();
        }

        sb.Append(char.ToUpperInvariant(" pnbrqk"[tur]));
        // Belirsizlik: aynı türden başka bir taş da aynı kareye gidebiliyor mu?
        var digerleri = YasalHamleler()
            .Where(m => m.Kime == h.Kime && m.Kimden != h.Kimden && _kare[m.Kimden] == tas).ToList();
        if (digerleri.Count > 0)
        {
            bool ayniSutun = digerleri.Any(m => Kare.Sutun(m.Kimden) == Kare.Sutun(h.Kimden));
            bool ayniSira = digerleri.Any(m => Kare.Sira(m.Kimden) == Kare.Sira(h.Kimden));
            if (!ayniSutun) sb.Append((char)('a' + Kare.Sutun(h.Kimden)));
            else if (!ayniSira) sb.Append((char)('1' + Kare.Sira(h.Kimden)));
            else sb.Append(Kare.Ad(h.Kimden));
        }
        if (alis) sb.Append('x');
        sb.Append(Kare.Ad(h.Kime));
        return sb.ToString();
    }

    /// <summary>SAN'ı (ya da UCI'yi) bu pozisyondaki yasal hamleye çevirir.</summary>
    public Hamle SanOku(string metin)
    {
        string s = metin.Trim().TrimEnd('+', '#', '!', '?').Replace("0-0-0", "O-O-O").Replace("0-0", "O-O");
        var yasal = YasalHamleler();

        foreach (var h in yasal)
            if (SanEksiz(h) == s || h.Uci == s) return h;

        // Esnek eşleşme: gereksiz belirsizlik gidermesi ("Ngf3"), eksik "x", "e8Q" gibi yazımlar
        var esnek = System.Text.RegularExpressions.Regex.Match(s,
            @"^([NBRQK])?([a-h])?([1-8])?x?([a-h][1-8])=?([NBRQnbrq])?$");
        if (esnek.Success)
        {
            int tur = esnek.Groups[1].Success ? " PNBRQK".IndexOf(esnek.Groups[1].Value[0]) : Tas.Piyon;
            int hedef = Kare.Oku(esnek.Groups[4].Value);
            int terfi = esnek.Groups[5].Success ? "pnbrqk".IndexOf(char.ToLowerInvariant(esnek.Groups[5].Value[0])) + 1 : 0;
            var adaylar = yasal.Where(h =>
                h.Kime == hedef && Tas.Tur(_kare[h.Kimden]) == tur && h.Terfi == terfi
                && (!esnek.Groups[2].Success || Kare.Sutun(h.Kimden) == esnek.Groups[2].Value[0] - 'a')
                && (!esnek.Groups[3].Success || Kare.Sira(h.Kimden) == esnek.Groups[3].Value[0] - '1')).ToList();
            if (adaylar.Count == 1) return adaylar[0];
        }
        throw new FormatException($"Geçersiz ya da yasal olmayan hamle: {metin} ({Fen()})");
    }

    public Hamle UciOku(string uci)
    {
        if (uci.Length < 4) return Hamle.Yok;
        int terfi = uci.Length > 4 ? "pnbrqk".IndexOf(uci[4]) + 1 : 0;
        return new Hamle(Kare.Oku(uci[..2]), Kare.Oku(uci[2..4]), terfi);
    }

    public IEnumerable<(int kare, int tas)> Taslar()
    {
        for (int k = 0; k < 64; k++)
            if (_kare[k] != 0) yield return (k, _kare[k]);
    }
}
