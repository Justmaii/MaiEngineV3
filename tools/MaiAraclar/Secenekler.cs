namespace MaiAraclar;

/// <summary>Basit "--ad değer" ayrıştırıcı; aynı ad birden çok kez verilebilir (--a-opt Hash=64 --a-opt Threads=1).</summary>
public sealed class Secenekler
{
    private readonly Dictionary<string, List<string>> _degerler = new();
    public List<string> Konumsal { get; } = new();

    public Secenekler(IEnumerable<string> args, params string[] bayraklar)
    {
        var liste = args.ToList();
        for (int i = 0; i < liste.Count; i++)
        {
            if (!liste[i].StartsWith("--")) { Konumsal.Add(liste[i]); continue; }
            string ad = liste[i][2..];
            string deger = !bayraklar.Contains(ad) && i + 1 < liste.Count ? liste[++i] : "";
            if (!_degerler.TryGetValue(ad, out var l)) _degerler[ad] = l = new();
            l.Add(deger);
        }
    }

    public string? Tek(string ad) => _degerler.TryGetValue(ad, out var l) ? l[^1] : null;
    public IEnumerable<string> Hepsi(string ad) => _degerler.TryGetValue(ad, out var l) ? l : Enumerable.Empty<string>();
    public bool Var(string ad) => _degerler.ContainsKey(ad);
}
