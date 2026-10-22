using System.Diagnostics;

namespace MaiAraclar;

/// <summary>Bir "go" aramasının sonucu: hamle ve son "info" satırındaki bilgiler.</summary>
public sealed record AramaSonucu(string EnIyi, int? Cp, int? Mat, int? Derinlik, long Dugum, int SureMs)
{
    /// <summary>Sıra gelen taraf açısından skor; mat 100000 - n (python-chess'in mate_score kuralı).</summary>
    public int? Sayi => Mat is int m ? (m > 0 ? 100000 - m : -100000 - m) : Cp;
}

/// <summary>UCI motoru ayrı süreçte çalıştırır. Motor kendi klasöründe başlar (ağ dosyası orada).</summary>
public sealed class UciMotor : IDisposable
{
    private readonly Process _surec;
    private readonly HashSet<string> _secenekler = new(StringComparer.OrdinalIgnoreCase);

    public UciMotor(string komut, IReadOnlyDictionary<string, string>? secenekler = null)
    {
        var parca = Bol(komut);
        string yol = Path.GetFullPath(parca[0]);
        var bilgi = new ProcessStartInfo(yol)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(yol)!,
        };
        foreach (var a in parca.Skip(1)) bilgi.ArgumentList.Add(a);
        _surec = Process.Start(bilgi) ?? throw new InvalidOperationException($"Motor başlatılamadı: {komut}");
        _surec.ErrorDataReceived += (_, _) => { };
        _surec.BeginErrorReadLine();
        _surec.StandardInput.AutoFlush = true;

        Gonder("uci");
        while (true)
        {
            string satir = Oku();
            if (satir == "uciok") break;
            if (satir.StartsWith("option name "))
            {
                int tur = satir.IndexOf(" type ", StringComparison.Ordinal);
                _secenekler.Add(satir[12..(tur < 0 ? satir.Length : tur)]);
            }
        }
        foreach (var (ad, deger) in secenekler ?? new Dictionary<string, string>())
            if (_secenekler.Contains(ad)) Gonder($"setoption name {ad} value {deger}");
        Hazir();
    }

    public void Gonder(string komut) => _surec.StandardInput.WriteLine(komut);

    private string Oku() => _surec.StandardOutput.ReadLine() ?? throw new InvalidOperationException("Motor kapandı");

    public void Hazir()
    {
        Gonder("isready");
        while (Oku() != "readyok") { }
    }

    public void YeniOyun() { Gonder("ucinewgame"); Hazir(); }

    /// <summary>"go ..." gönderir, bestmove gelene kadar bekler.</summary>
    public AramaSonucu Ara(string pozisyon, string go)
    {
        Gonder(pozisyon);
        var saat = Stopwatch.StartNew();
        Gonder(go);
        int? cp = null, mat = null, derinlik = null, sure = null;
        long dugum = 0;
        while (true)
        {
            string satir = Oku();
            if (satir.StartsWith("bestmove"))
            {
                var p = satir.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                return new AramaSonucu(p.Length > 1 ? p[1] : "0000", cp, mat, derinlik, dugum,
                                       sure ?? (int)saat.ElapsedMilliseconds);
            }
            if (!satir.StartsWith("info ")) continue;
            var t = satir.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            for (int i = 1; i + 1 < t.Length; i++)
            {
                switch (t[i])
                {
                    case "depth": derinlik = int.Parse(t[++i]); break;
                    case "nodes": dugum = long.Parse(t[++i]); break;
                    case "time": sure = int.Parse(t[++i]); break;
                    case "score" when i + 2 < t.Length:
                        if (t[i + 1] == "mate") { mat = int.Parse(t[i + 2]); cp = null; }
                        else if (t[i + 1] == "cp") { cp = int.Parse(t[i + 2]); mat = null; }
                        i += 2;
                        break;
                    case "pv": case "string": i = t.Length; break;
                }
            }
        }
    }

    /// <summary>Komutu kabuk gibi böler (tırnaklar desteklenir).</summary>
    private static List<string> Bol(string komut)
    {
        var sonuc = new List<string>();
        var sb = new System.Text.StringBuilder();
        char? tirnak = null;
        foreach (char c in komut)
        {
            if (tirnak != null) { if (c == tirnak) tirnak = null; else sb.Append(c); }
            else if (c is '"' or '\'') tirnak = c;
            else if (char.IsWhiteSpace(c)) { if (sb.Length > 0) { sonuc.Add(sb.ToString()); sb.Clear(); } }
            else sb.Append(c);
        }
        if (sb.Length > 0) sonuc.Add(sb.ToString());
        return sonuc;
    }

    public void Dispose()
    {
        try
        {
            Gonder("quit");
            if (!_surec.WaitForExit(2000)) _surec.Kill();
        }
        catch { /* zaten kapanmış */ }
        _surec.Dispose();
    }
}
