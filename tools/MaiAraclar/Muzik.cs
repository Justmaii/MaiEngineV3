using System.Globalization;

namespace MaiAraclar;

/// <summary>
/// Tanıtım videosu için telifsiz, sıfırdan sentezlenen fon müziği; dışarıdan ses dosyası yok.
///
///     dotnet run --project tools/MaiAraclar -c Release -- muzik &lt;süre_sn&gt; &lt;çıktı.wav&gt; [geçiş zamanları,] [vurgu zamanları,]
///
/// 120 BPM, Am - F - C - G; davul girişi 3.9 sn'de, geçişlerde "vuus", vurgularda derin darbe.
/// </summary>
public static class Muzik
{
    private const int SR = 44100;

    private sealed class Karisim(int n)
    {
        public readonly double[] L = new double[n], R = new double[n];

        public void Ekle(double[] ses, double t0, double kazanc = 1.0, double pan = 0.0)
        {
            int i = (int)(t0 * SR);
            if (i >= L.Length || i < 0 && -i >= ses.Length) return;
            double sol = kazanc * Math.Sqrt(1 - pan), sag = kazanc * Math.Sqrt(1 + pan);
            for (int k = 0; k < ses.Length && i + k < L.Length; k++)
            {
                if (i + k < 0) continue;
                L[i + k] += ses[k] * sol;
                R[i + k] += ses[k] * sag;
            }
        }
    }

    private static readonly Random Rng = new(7);

    private static double Normal()
    {
        // Box-Muller
        double u1 = 1.0 - Rng.NextDouble(), u2 = Rng.NextDouble();
        return Math.Sqrt(-2.0 * Math.Log(u1)) * Math.Cos(2 * Math.PI * u2);
    }

    private static double[] Gurultu(int n) => Enumerable.Range(0, n).Select(_ => Normal()).ToArray();

    /// <summary>Birinci fark (np.diff ile başına 0 eklenmiş): basit yüksek geçiren.</summary>
    private static double[] Fark(double[] x)
    {
        var y = new double[x.Length];
        for (int i = 0; i < x.Length; i++) y[i] = x[i] - (i > 0 ? x[i - 1] : 0);
        return y;
    }

    private static double Midi(int m) => 440 * Math.Pow(2, (m - 69) / 12.0);

    private static double[] Davul()
    {
        int n = (int)(0.45 * SR);
        var s = new double[n];
        double faz = 0;
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / SR;
            faz += 2 * Math.PI * (42 + 110 * Math.Exp(-t * 28)) / SR;
            s[i] = Math.Sin(faz) * Math.Exp(-t * 7) * 0.95;
        }
        return s;
    }

    private static double[] Zil(bool acik = false)
    {
        int n = (int)((acik ? 0.25 : 0.06) * SR);
        var x = Fark(Gurultu(n));
        for (int i = 0; i < n; i++) x[i] *= Math.Exp(-(double)i / SR * (acik ? 14 : 70)) * 0.18;
        return x;
    }

    private static double[] El()
    {
        int n = (int)(0.3 * SR);
        var x = Fark(Gurultu(n));
        var e = new double[n];
        for (int i = 0; i < n; i++) e[i] = Math.Exp(-(double)i / SR * 22);
        foreach (double k in new[] { 0.0, 0.012, 0.024 })
            for (int i = (int)(k * SR); i < Math.Min(n, (int)(k * SR) + 200); i++) e[i] += 0.6;
        for (int i = 0; i < n; i++) x[i] *= e[i] * 0.22;
        return x;
    }

    private static double[] Testere(double frekans, int n, double detune)
    {
        var s = new double[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / SR, toplam = 0;
            foreach (double d in new[] { -detune, 0, detune })
            {
                double faz = t * frekans * (1 + d);
                toplam += 2 * (faz - Math.Floor(faz)) - 1;
            }
            s[i] = toplam / 3;
        }
        return s;
    }

    /// <summary>Tek kutuplu alçak geçiren, iki kez: yumuşak.</summary>
    private static double[] AlcakGecir(double[] x, double kesim)
    {
        double a = Math.Exp(-2 * Math.PI * kesim / SR);
        var y = (double[])x.Clone();
        for (int pas = 0; pas < 2; pas++)
        {
            double acc = 0;
            for (int i = 0; i < y.Length; i++) { acc = (1 - a) * y[i] + a * acc; y[i] = acc; }
        }
        return y;
    }

    private static double[] Ped(int[] akor, int n)
    {
        var s = new double[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / SR, toplam = 0;
            foreach (int m in akor)
                toplam += Math.Sin(2 * Math.PI * Midi(m) * t) + 0.35 * Math.Sin(2 * Math.PI * Midi(m) * 2.003 * t);
            double e = Math.Min(1, t / 0.4) * Math.Min(1, ((double)n / SR - t) / 0.4);
            s[i] = toplam * e / akor.Length * 0.22;
        }
        return s;
    }

    private static double[] Mizrap(int m, int n)
    {
        var s = AlcakGecir(Testere(Midi(m), n, 0.004), 2600);
        for (int i = 0; i < n; i++) s[i] *= Math.Exp(-(double)i / SR * 9) * 0.16;
        return s;
    }

    private static double[] Bas(int m, int n)
    {
        var s = new double[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / SR;
            double v = Math.Sin(2 * Math.PI * Midi(m) * t) + 0.3 * Math.Sin(2 * Math.PI * Midi(m) * 2 * t);
            s[i] = v * Math.Min(1, t / 0.005) * Math.Exp(-t * 3.5) * 0.34;
        }
        return s;
    }

    public static int Calistir(string[] args)
    {
        double sure = double.Parse(args[0], CultureInfo.InvariantCulture);
        string cikti = args[1];
        double[] Zamanlar(int i) => args.Length > i && args[i].Length > 0
            ? args[i].Split(',').Select(x => double.Parse(x, CultureInfo.InvariantCulture)).ToArray() : [];
        var gecisler = Zamanlar(2);
        var vurgular = Zamanlar(3);

        int N = (int)(SR * sure);
        var mix = new Karisim(N);
        const double bpm = 120, vurus = 60 / bpm, olcu = 4 * vurus;

        // Akor dizisi: Am - F - C - G (her biri bir ölçü)
        int[][] akorlar = { new[] { 57, 60, 64 }, new[] { 53, 57, 60 }, new[] { 48, 52, 55 }, new[] { 55, 59, 62 } };
        int[] baslar = { 45, 41, 48, 43 };
        int[][] arpej = { new[] { 69, 72, 76, 72 }, new[] { 65, 69, 72, 69 }, new[] { 64, 67, 72, 67 }, new[] { 67, 71, 74, 71 } };

        const double giris = 3.9;   // davul girişi
        int olcuSayisi = (int)(sure / olcu) + 1;
        var mizrapOnbellek = new Dictionary<int, double[]>();
        for (int b = 0; b < olcuSayisi; b++)
        {
            double t0 = b * olcu;
            int c = b % 4;
            mix.Ekle(Ped(akorlar[c], (int)(olcu * SR)), t0);
            for (int k = 0; k < 4; k++)
            {
                double tb = t0 + k * vurus;
                if (tb >= sure) break;
                // bas: vuruşların arasında (sidechain hissi)
                if (tb >= giris - 0.01)
                {
                    mix.Ekle(Bas(baslar[c], (int)(vurus * 0.9 * SR)), tb + vurus / 2);
                    mix.Ekle(Davul(), tb);
                    if (k is 1 or 3) mix.Ekle(El(), tb, 1.0, 0.1);
                    mix.Ekle(Zil(), tb + vurus / 2, 1.0, 0.35);
                    mix.Ekle(Zil(), tb + vurus / 4, 0.5, -0.35);
                    mix.Ekle(Zil(), tb + 3 * vurus / 4, 0.5, -0.35);
                }
                // arpej 16'lık
                for (int s = 0; s < 4; s++)
                {
                    double ts = tb + s * vurus / 4;
                    if (ts >= sure) break;
                    int m = arpej[c][s] + ((b / 4) % 2 == 1 ? 12 : 0);
                    if (!mizrapOnbellek.TryGetValue(m, out var ses)) mizrapOnbellek[m] = ses = Mizrap(m, (int)(0.3 * SR));
                    mix.Ekle(ses, ts, ts >= giris ? 0.8 : 0.45, s % 2 == 1 ? 0.3 : -0.3);
                }
            }
        }

        // Geçişlerde "vuus" (yükselen filtreli gürültü)
        foreach (double w in gecisler)
        {
            int n = (int)(0.8 * SR);
            var x = Fark(Gurultu(n));
            for (int i = 0; i < n; i++)
            {
                double e = Math.Pow(Math.Sin(Math.PI * Math.Clamp((double)i / SR / 0.8, 0, 1)), 2);
                x[i] *= e * 0.12;
            }
            mix.Ekle(x, w - 0.35);
        }
        // Vurgular (MAT anı vb.): derin darbe
        foreach (double h in vurgular)
        {
            int n = (int)(1.2 * SR);
            var s = new double[n];
            double faz = 0;
            for (int i = 0; i < n; i++)
            {
                double t = (double)i / SR;
                faz += 2 * Math.PI * (38 + 80 * Math.Exp(-t * 10)) / SR;
                s[i] = Math.Sin(faz) * Math.Exp(-t * 3) * 0.9 + Normal() * Math.Exp(-t * 18) * 0.15;
            }
            mix.Ekle(s, h);
        }

        // Kapanış: son 3 sn sönme, başta kısa açılış; yumuşak limitleyici ve normalizasyon
        double tepe = 1e-9;
        for (int i = 0; i < N; i++)
        {
            double t = (double)i / SR;
            double sonme = Math.Min(1, t / 0.6) * Math.Clamp((sure - t) / 3.0, 0, 1);
            mix.L[i] = Math.Tanh(mix.L[i] * sonme * 1.1) * 0.9;
            mix.R[i] = Math.Tanh(mix.R[i] * sonme * 1.1) * 0.9;
            tepe = Math.Max(tepe, Math.Max(Math.Abs(mix.L[i]), Math.Abs(mix.R[i])));
        }

        using var w2 = new BinaryWriter(File.Create(cikti));
        int veriBoyu = N * 4;
        w2.Write("RIFF"u8); w2.Write(36 + veriBoyu); w2.Write("WAVE"u8);
        w2.Write("fmt "u8); w2.Write(16); w2.Write((short)1); w2.Write((short)2);
        w2.Write(SR); w2.Write(SR * 4); w2.Write((short)4); w2.Write((short)16);
        w2.Write("data"u8); w2.Write(veriBoyu);
        for (int i = 0; i < N; i++)
        {
            w2.Write((short)(mix.L[i] / tepe * 0.89 * 32767));
            w2.Write((short)(mix.R[i] / tepe * 0.89 * 32767));
        }
        Console.WriteLine($"yazildi {cikti} {sure.ToString(CultureInfo.InvariantCulture)} sn");
        return 0;
    }
}
