using System.Globalization;

namespace MaiAraclar;

/// <summary>
/// Motorların arama hızını aynı koşulda ölçer: tek iş parçacığı, Hash 64,
/// her pozisyonda sabit süre düşünme, son "info" satırındaki düğüm/süre.
///
///     dotnet run --project tools/MaiAraclar -c Release -- hiz 10 ./maiengine ../opp/inphish5 ...
/// </summary>
public static class Hiz
{
    private static readonly string[] Fenler =
    {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
    };

    public static int Calistir(string[] args)
    {
        double saniye = double.Parse(args[0], CultureInfo.InvariantCulture);
        foreach (var komut in args.Skip(1))
        {
            using var motor = new UciMotor(komut, new Dictionary<string, string> { ["Threads"] = "1", ["Hash"] = "64" });
            long dugum = 0, sureMs = 0;
            foreach (var fen in Fenler)
            {
                var r = motor.Ara("position fen " + fen, $"go movetime {(int)(saniye * 1000)}");
                dugum += r.Dugum;
                sureMs += r.SureMs;
            }
            Console.WriteLine($"{Path.GetFileName(komut)}\t{(long)(dugum / (sureMs / 1000.0))}");
        }
        return 0;
    }
}
