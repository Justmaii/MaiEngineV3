using MaiAraclar;

// MaiEngine geliştirme araçları (C#):
//   mac         UCI motor maçı (gerçek saat, paralel oyunlar, Elo)
//   hiz         motorların arama hızı (nps)
//   perft-uret  rastgele pozisyonlardan perft EPD üretir
//   muzik       tanıtım videosunun fon müziği (wav)
//
//   dotnet run --project tools/MaiAraclar -c Release -- <komut> ...

if (args.Length == 0)
{
    Console.WriteLine("Kullanım: MaiAraclar <mac|hiz|perft-uret|muzik> ...  (ayrıntı: kaynak dosyaların başındaki açıklamalar)");
    return 1;
}

var geri = args.Skip(1).ToArray();
return args[0] switch
{
    "mac" => Mac.Calistir(new Secenekler(geri, "adjudicate")),
    "hiz" => Hiz.Calistir(geri),
    "perft-uret" => PerftUret.Calistir(geri),
    "muzik" => Muzik.Calistir(geri),
    _ => throw new ArgumentException($"Bilinmeyen komut: {args[0]}"),
};
