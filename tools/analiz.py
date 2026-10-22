#!/usr/bin/env python3
"""MaiEngine oyun analizi — chess.com "Oyun İncelemesi" tarzı.

PGN'deki her pozisyonu MaiEngine ile analiz eder, her hamleyi sınıflandırır
(Parlak, Harika, En İyi, Mükemmel, İyi, Yanlışlık, Hata, Kaçırma, Büyük Hata)
ve interaktif bir HTML rapor üretir.

Kullanım:
    tools/.venv/bin/python tools/analiz.py oyun.pgn
    tools/.venv/bin/python tools/analiz.py oyun.pgn --sure 1.0 --threads 8 -o rapor.html

Sınıflandırma, Lichess/chess.com'un kullandığı "kazanma olasılığı kaybı"
modeline dayanır: hamleden önceki en iyi devamın kazanma yüzdesi ile oynanan
hamlenin kazanma yüzdesi arasındaki fark.
"""

import argparse
import base64
import json
import math
import os
import sys
import time

import chess
import chess.engine
import chess.pgn
import chess.polyglot
import chess.svg

KOK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VARSAYILAN_MOTOR = os.path.join(KOK, "maiengine")
VARSAYILAN_KITAP = os.path.join(KOK, "book.bin")

MAT_CP = 100000
DEGER = {chess.PAWN: 1, chess.KNIGHT: 3, chess.BISHOP: 3, chess.ROOK: 5, chess.QUEEN: 9, chess.KING: 0}

# Sınıf anahtarı -> (görünen ad, simge, renk)
SINIFLAR = {
    "parlak":    ("Parlak",     "!!", "#1baca6"),
    "harika":    ("Harika",     "!",  "#5c8bb0"),
    "eniyi":     ("En İyi",     "★",  "#81b64c"),
    "mukemmel":  ("Mükemmel",   "👍", "#96bc4b"),
    "iyi":       ("İyi",        "✓",  "#95af8a"),
    "kitap":     ("Kitap",      "📖", "#a88865"),
    "zorunlu":   ("Zorunlu",    "□",  "#97a5a8"),
    "yanlislik": ("Yanlışlık",  "?!", "#f7c631"),
    "hata":      ("Hata",       "?",  "#ffa459"),
    "kacirma":   ("Kaçırma",    "✕",  "#ff7769"),
    "buyukhata": ("Büyük Hata", "??", "#fa412d"),
}
SINIF_SIRASI = list(SINIFLAR)


def kazanma(cp):
    """Santipiyondan kazanma olasılığına (0..1), Lichess formülü."""
    cp = max(-MAT_CP, min(MAT_CP, cp))
    return 1 / (1 + math.exp(-0.00368208 * cp))


def hamle_dogrulugu(once, sonra):
    """Kazanma olasılığı düşüşünden hamle doğruluğu (0..100), Lichess formülü."""
    fark = max(0.0, once - sonra) * 100
    return max(0.0, min(100.0, 103.1668 * math.exp(-0.04354 * fark) - 3.1669))


def oyun_dogrulugu(liste):
    if not liste:
        return None
    aritmetik = sum(liste) / len(liste)
    harmonik = len(liste) / sum(1 / max(a, 1.0) for a in liste)
    return (aritmetik + harmonik) / 2


def skor_cp(skor):
    return skor.score(mate_score=MAT_CP)


def skor_metni(skor):
    """Beyaz bakış açısından skor metni: +1.25, -0.40, M3, -M2."""
    if skor.is_mate():
        m = skor.mate()
        if m == 0:
            return "#"
        return ("M" if m > 0 else "-M") + str(abs(m))
    return f"{skor.score() / 100:+.2f}"


def asili_kazanc(tahta, ben):
    """Sıra rakipteyken, rakibin bizim piyon dışı taşlarımızdan birini alarak
    kazanabileceği en büyük net malzeme (kaba SEE)."""
    en_buyuk = 0
    for kare, tas in tahta.piece_map().items():
        if tas.color != ben or tas.piece_type in (chess.PAWN, chess.KING):
            continue
        alicilar = [m for m in tahta.legal_moves if m.to_square == kare]
        if not alicilar:
            continue
        en_ucuz = min(DEGER[tahta.piece_type_at(m.from_square)] for m in alicilar)
        korunuyor = bool(tahta.attackers(ben, kare))
        kazanc = DEGER[tas.piece_type] - en_ucuz if korunuyor else DEGER[tas.piece_type]
        en_buyuk = max(en_buyuk, kazanc)
    return en_buyuk


def bedava_alis(tahta, hamle):
    """Korumasız ya da alan taştan değerli bir taşı almak — bulması kolay bir hamle."""
    if not tahta.is_capture(hamle) or tahta.is_en_passant(hamle):
        return False
    alinan = tahta.piece_type_at(hamle.to_square)
    alan = tahta.piece_type_at(hamle.from_square)
    return not tahta.attackers(not tahta.turn, hamle.to_square) or DEGER[alinan] > DEGER[alan]


def kurban_mi(once, hamle):
    """Hamle bir taş kurbanı mı? Hamleden sonra rakip, hamlede aldığımızdan en az
    2 puan fazla malzeme kazanabiliyorsa ve bu durum hamleyle yeni oluştuysa
    (zaten asılı olan bir taşı görmezden gelmek sayılmaz)."""
    ben = once.turn
    alinan = 0
    if once.is_en_passant(hamle):
        alinan = 1
    else:
        hedef = once.piece_at(hamle.to_square)
        if hedef:
            alinan = DEGER[hedef.piece_type]
    if hamle.promotion:
        alinan += DEGER[hamle.promotion] - 1

    sonra = once.copy(stack=False)
    sonra.push(hamle)
    if sonra.is_checkmate():
        return False
    kazanc_sonra = asili_kazanc(sonra, ben)

    kazanc_once = 0
    if not once.is_check():
        bos = once.copy(stack=False)
        bos.push(chess.Move.null())
        kazanc_once = asili_kazanc(bos, ben)
    return kazanc_sonra - alinan >= 2 and kazanc_sonra > kazanc_once


class Analizci:
    def __init__(self, motor, limit, threads, hash_mb):
        self.motor = chess.engine.SimpleEngine.popen_uci(motor)
        self.motor.configure({"Threads": threads, "Hash": hash_mb})
        self.limit = limit

    def kapat(self):
        self.motor.quit()

    def analiz(self, tahta, oyun, kok_hamleler=None):
        """Sıra gelen taraf açısından (cp, Score[beyaz], en iyi hamle, pv)."""
        if tahta.is_checkmate():
            s = chess.engine.PovScore(chess.engine.Mate(0), tahta.turn)
            return -MAT_CP, s.white(), None, []
        if tahta.is_stalemate() or tahta.is_insufficient_material():
            s = chess.engine.PovScore(chess.engine.Cp(0), tahta.turn)
            return 0, s.white(), None, []
        bilgi = self.motor.analyse(tahta, self.limit, game=oyun, root_moves=kok_hamleler)
        pov = bilgi["score"]
        pv = bilgi.get("pv", [])
        return skor_cp(pov.pov(tahta.turn)), pov.white(), (pv[0] if pv else None), pv


def eniyi_fen(tahta, hamle):
    if not hamle:
        return None
    t = tahta.copy(stack=False)
    t.push(hamle)
    return t.fen()


def konum_anahtari(tahta):
    return " ".join(tahta.fen().split()[:4])


def san_dizisi(tahta, hamleler, n=8):
    t = tahta.copy(stack=False)
    sonuc = []
    for m in hamleler[:n]:
        if m not in t.legal_moves:
            break
        sonuc.append(t.san(m))
        t.push(m)
    return sonuc


def kitapta_mi(kitap, tahta, hamle):
    """Hamle kitapta mı? Ya doğrudan kitap hamlesi ya da kitaptaki bir pozisyona
    geçiş (transpozisyon) yapıyor."""
    if any(e.move == hamle for e in kitap.find_all(tahta)):
        return True
    sonra = tahta.copy(stack=False)
    sonra.push(hamle)
    return kitap.get(sonra) is not None


def oyunu_analiz_et(oyun, analizci, hizli, ilerleme, kitap=None):
    tahta = oyun.board()
    hamleler = list(oyun.mainline_moves())
    tahtalar = [tahta.copy()]
    for m in hamleler:
        tahta.push(m)
        tahtalar.append(tahta.copy())

    # Her pozisyonu bir kez analiz et: hamle i'nin "sonrası", hamle i+1'in "öncesi".
    pozlar = []
    for i, t in enumerate(tahtalar):
        ilerleme(i, 2 * len(tahtalar) - 1)
        cp, beyaz, eniyi, pv = analizci.analiz(t, oyun)
        pozlar.append({"cp": cp, "beyaz": beyaz, "eniyi": eniyi, "pv": pv})

    plylar = []
    onceki_sinif = None
    gorulen = set()
    kitapta = kitap is not None
    for i, m in enumerate(hamleler):
        ilerleme(len(tahtalar) + i, 2 * len(tahtalar) - 1)
        once = tahtalar[i]
        ben = once.turn
        p_once, p_sonra = pozlar[i], pozlar[i + 1]
        eniyi = p_once["eniyi"]
        en_iyi_cp = p_once["cp"]
        yasal = once.legal_moves.count()

        if m == eniyi:
            oynanan_cp = en_iyi_cp
        else:
            oynanan_cp = -p_sonra["cp"]
        k_once, k_oynanan = kazanma(en_iyi_cp), kazanma(oynanan_cp)
        kayip = max(0.0, k_once - k_oynanan)

        # Kitap hamlesi motor gözünde kötüyse (tuzak hatları da kitapta) yine hata sayılır.
        kitapta = kitapta and kayip <= 0.05 and kitapta_mi(kitap, once, m)
        if kitapta:
            sinif = "kitap"
        elif yasal == 1:
            sinif = "zorunlu"
        elif m == eniyi or kayip < 0.005:
            sinif = "eniyi"
        elif kayip <= 0.02:
            sinif = "mukemmel"
        elif kayip <= 0.05:
            sinif = "iyi"
        elif kayip <= 0.10:
            sinif = "yanlislik"
        elif kayip <= 0.20:
            sinif = "hata"
        else:
            sinif = "buyukhata"

        # Kaçırma: rakip hata yaptı, fırsat vardı, sen kaçırdın ama oyun çökmedi.
        if (sinif in ("hata", "buyukhata") and onceki_sinif in ("hata", "buyukhata", "kacirma")
                and k_once >= 0.70 and k_oynanan >= 0.35):
            sinif = "kacirma"

        # Parlak: en iyi (veya neredeyse) hamle + gerçek bir taş kurbanı,
        # sonrasında kötü durumda değilsin ve zaten tamamen kazanmış değildin.
        if (sinif in ("eniyi", "mukemmel") and kayip <= 0.02 and 0.45 <= k_oynanan
                and k_once < 0.90 and kurban_mi(once, m)):
            sinif = "parlak"

        # Harika: tek iyi hamle — ikinci en iyi hamle belirgin şekilde daha kötü.
        # Basit geri almalar, şahtan kaçışlar ve tekrar eden pozisyonlar sayılmaz.
        ikinci_metin = None
        if sinif == "eniyi" and m == eniyi and not hizli and yasal > 1:
            geri_alma = (i > 0 and once.is_capture(m) and m.to_square == hamleler[i - 1].to_square)
            tekrar = konum_anahtari(once) in gorulen
            if not (geri_alma or tekrar or once.is_check() or bedava_alis(once, m)):
                digerleri = [x for x in once.legal_moves if x != m]
                cp2, beyaz2, _, pv2 = analizci.analiz(once, oyun, kok_hamleler=digerleri)
                if pv2:
                    ikinci_metin = once.san(pv2[0]) + " " + skor_metni(beyaz2)
                if k_once - kazanma(cp2) >= 0.20 and k_once >= 0.30:
                    sinif = "harika"

        dogruluk = hamle_dogrulugu(k_once, k_oynanan)
        pv_san = san_dizisi(once, p_once["pv"])
        plylar.append({
            "san": once.san(m),
            "uci": m.uci(),
            "renk": "w" if ben == chess.WHITE else "b",
            "no": once.fullmove_number,
            "sinif": sinif,
            "eniyi_san": once.san(eniyi) if eniyi else None,
            "eniyi_uci": eniyi.uci() if eniyi else None,
            "eniyi_fen": eniyi_fen(once, eniyi),
            "eniyi_skor": skor_metni(p_once["beyaz"]),
            "pv": pv_san,
            "ikinci": ikinci_metin,
            "skor": skor_metni(p_sonra["beyaz"]),
            "kazanma_beyaz": kazanma(skor_cp(p_sonra["beyaz"])),
            "kayip": round(kayip * 100, 1),
            "dogruluk": round(dogruluk, 1),
            "fen": tahtalar[i + 1].fen(),
        })
        onceki_sinif = sinif
        gorulen.add(konum_anahtari(once))

    dogruluk = {}
    for renk in ("w", "b"):
        liste = [p["dogruluk"] for p in plylar if p["renk"] == renk and p["sinif"] not in ("zorunlu", "kitap")]
        d = oyun_dogrulugu(liste)
        dogruluk[renk] = round(d, 1) if d is not None else None

    sayac = {renk: {s: 0 for s in SINIF_SIRASI} for renk in ("w", "b")}
    for p in plylar:
        sayac[p["renk"]][p["sinif"]] += 1

    h = oyun.headers
    return {
        "beyaz": h.get("White", "?") if h.get("White", "?") != "?" else "Beyaz",
        "siyah": h.get("Black", "?") if h.get("Black", "?") != "?" else "Siyah",
        "sonuc": h.get("Result", "*"),
        "etkinlik": h.get("Event", ""),
        "tarih": h.get("Date", ""),
        "baslangic_fen": tahtalar[0].fen(),
        "baslangic_kazanma": kazanma(skor_cp(pozlar[0]["beyaz"])),
        "baslangic_skor": skor_metni(pozlar[0]["beyaz"]),
        "plylar": plylar,
        "dogruluk": dogruluk,
        "sayac": sayac,
    }


def tas_resimleri():
    resimler = {}
    for s in "PNBRQKpnbrqk":
        svg = chess.svg.piece(chess.Piece.from_symbol(s))
        resimler[s] = "data:image/svg+xml;base64," + base64.b64encode(svg.encode()).decode()
    return resimler


def html_uret(oyunlar, baslik, geri=None):
    veri = json.dumps({"oyunlar": oyunlar, "siniflar": SINIFLAR, "sira": SINIF_SIRASI,
                       "taslar": tas_resimleri()}, ensure_ascii=False)
    veri = veri.replace("</", "<\\/")
    geri_html = f'<a class="geri" href="{geri}">← Yeni analiz</a>' if geri else ""
    return (SABLON.replace("__GERI__", geri_html)
            .replace("__BASLIK__", json.dumps(baslik).replace("</", "<\\/"))
            .replace("__VERI__", veri))


def terminal_ozeti(o):
    print(f"\n  {o['beyaz']} vs {o['siyah']}  {o['sonuc']}")
    print(f"  Doğruluk: Beyaz {o['dogruluk']['w']}  Siyah {o['dogruluk']['b']}")
    for s in SINIF_SIRASI:
        b, w = o["sayac"]["w"][s], o["sayac"]["b"][s]
        if b or w:
            print(f"    {SINIFLAR[s][0]:<11} {b:>3}  {w:>3}")
    for p in o["plylar"]:
        if p["sinif"] in ("parlak", "harika", "kacirma", "hata", "buyukhata"):
            nokta = "." if p["renk"] == "w" else "..."
            ek = f"  (en iyisi {p['eniyi_san']})" if p["sinif"] not in ("parlak", "harika") else ""
            print(f"    {p['no']}{nokta}{p['san']:<8} {SINIFLAR[p['sinif']][0]}{ek}")


def main():
    ap = argparse.ArgumentParser(description="MaiEngine ile PGN oyun analizi (chess.com tarzı)")
    ap.add_argument("pgn", help="analiz edilecek PGN dosyası")
    ap.add_argument("-o", "--cikti", help="HTML rapor yolu (varsayılan: <pgn>.analiz.html)")
    ap.add_argument("--motor", default=VARSAYILAN_MOTOR, help="UCI motor yolu")
    ap.add_argument("--sure", type=float, default=0.5, help="pozisyon başına süre (sn), varsayılan 0.5")
    ap.add_argument("--derinlik", type=int, help="süre yerine sabit derinlik")
    ap.add_argument("--threads", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--hash", type=int, default=256, help="hash (MB)")
    ap.add_argument("--oyunlar", help="sadece bu oyunlar, örn. 1,3,5 (1'den başlar)")
    ap.add_argument("--hizli", action="store_true", help="'Harika' için ek arama yapma")
    ap.add_argument("--kitap", default=VARSAYILAN_KITAP, help="Polyglot açılış kitabı (.bin)")
    ap.add_argument("--kitapsiz", action="store_true", help="açılış kitabını kullanma")
    a = ap.parse_args()

    if not os.path.exists(a.motor):
        sys.exit(f"Motor bulunamadı: {a.motor}  (önce 'make' ile derle)")

    secili = None
    if a.oyunlar:
        secili = {int(x) for x in a.oyunlar.split(",") if x.strip()}

    limit = chess.engine.Limit(depth=a.derinlik) if a.derinlik else chess.engine.Limit(time=a.sure)
    analizci = Analizci(a.motor, limit, a.threads, a.hash)
    kitap = None
    if not a.kitapsiz and os.path.exists(a.kitap):
        kitap = chess.polyglot.open_reader(a.kitap)
    sonuclar = []
    try:
        with open(a.pgn, encoding="utf-8", errors="replace") as f:
            no = 0
            while True:
                oyun = chess.pgn.read_game(f)
                if oyun is None:
                    break
                no += 1
                if secili and no not in secili:
                    continue
                if oyun.end().ply() == oyun.ply():
                    continue
                basla = time.time()

                def ilerleme(i, n, no=no):
                    print(f"\r  Oyun {no}: adım {i + 1}/{n}", end="", flush=True)

                o = oyunu_analiz_et(oyun, analizci, a.hizli, ilerleme, kitap)
                o["no"] = no
                print(f"  ({time.time() - basla:.0f} sn)", end="")
                terminal_ozeti(o)
                sonuclar.append(o)
    finally:
        analizci.kapat()
        if kitap:
            kitap.close()

    if not sonuclar:
        sys.exit("PGN'de analiz edilecek oyun yok.")

    cikti = a.cikti or os.path.splitext(a.pgn)[0] + ".analiz.html"
    baslik = os.path.basename(a.pgn)
    with open(cikti, "w", encoding="utf-8") as f:
        f.write(html_uret(sonuclar, baslik))
    print(f"\nRapor: {cikti}")


SABLON = r"""<!doctype html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MaiEngine Oyun İncelemesi</title>
<style>
:root {
  --bg: #f4f2ee; --panel: #ffffff; --text: #23211e; --muted: #6f6a62; --line: #e3dfd8;
  --light: #ebecd0; --dark: #739552; --hl: rgba(255, 220, 60, .5); --sel: #e9e4da;
  --arrow: rgba(129, 182, 76, .9);
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #262421; --panel: #312e2b; --text: #ece9e4; --muted: #a29d95; --line: #403d39;
    --sel: #45413c;
  }
}
:root[data-theme="dark"] {
  --bg: #262421; --panel: #312e2b; --text: #ece9e4; --muted: #a29d95; --line: #403d39; --sel: #45413c;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
  font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
header { padding: 14px 16px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
header h1 { font-family: "Iowan Old Style", "Palatino Linotype", Georgia, serif; font-weight: 600;
  font-size: 20px; margin: 0; flex: 1; }
header .geri { color: var(--muted); text-decoration: none; font-weight: 600; }
header .geri:hover { color: var(--text); }
select, button { font: inherit; color: var(--text); background: var(--panel); border: 1px solid var(--line);
  border-radius: 6px; padding: 6px 10px; cursor: pointer; }
button:hover { background: var(--sel); }
main { display: grid; grid-template-columns: minmax(0, 560px) minmax(0, 1fr); gap: 16px; padding: 0 16px 24px;
  max-width: 1200px; margin: 0 auto; }
@media (max-width: 860px) { main { grid-template-columns: minmax(0, 1fr); } }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 12px; }
.oyuncu { display: flex; justify-content: space-between; font-weight: 600; padding: 4px 2px; }
.tahta-sar { display: flex; gap: 8px; }
.evalbar { width: 22px; border-radius: 4px; overflow: hidden; background: #403d39; position: relative; flex: none; }
.evalbar .beyaz { position: absolute; left: 0; right: 0; bottom: 0; background: #f0efec; transition: height .25s; }
.evalbar .yazi { position: absolute; left: 0; right: 0; font-size: 10px; text-align: center; font-weight: 700; }
.tahta { position: relative; flex: 1; aspect-ratio: 1; display: grid; grid-template-columns: repeat(8, 1fr);
  border-radius: 4px; overflow: hidden; }
.kare { position: relative; }
.kare.a { background: var(--light); } .kare.k { background: var(--dark); }
.kare.vurgu::before { content: ""; position: absolute; inset: 0; background: var(--hl); }
.kare img { position: absolute; inset: 0; width: 100%; height: 100%; }
.kare .koord { position: absolute; font-size: 10px; font-weight: 700; z-index: 1; }
.kare.a .koord { color: var(--dark); } .kare.k .koord { color: var(--light); }
.koord.sira { left: 3px; top: 1px; } .koord.sutun { right: 3px; bottom: 0; }
.rozet { position: absolute; top: -6%; right: -6%; width: 42%; height: 42%; border-radius: 50%; z-index: 3;
  color: #fff; font-weight: 800; display: flex; align-items: center; justify-content: center;
  font-size: clamp(9px, 1.6vw, 14px); box-shadow: 0 1px 3px rgba(0,0,0,.4); }
svg.oklar { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; z-index: 2; }
.kontrol { display: flex; gap: 6px; justify-content: center; margin-top: 10px; }
.kontrol button { min-width: 48px; }
.aciklama { margin-top: 10px; min-height: 64px; }
.aciklama .bas { font-weight: 700; font-size: 15px; display: flex; gap: 8px; align-items: center; }
.aciklama .pv { color: var(--muted); font-size: 13px; margin-top: 4px; }
.eniyi-dugme { margin-top: 10px; border-color: #81b64c; color: #81b64c; font-weight: 600; }
.eniyi-dugme:hover { background: rgba(129, 182, 76, .12); }
.simge { display: inline-flex; align-items: center; justify-content: center; width: 20px; height: 20px;
  border-radius: 50%; color: #fff; font-size: 11px; font-weight: 800; flex: none; }
.ozet table { width: 100%; border-collapse: collapse; }
.ozet td, .ozet th { padding: 3px 6px; text-align: center; }
.ozet td:first-child, .ozet th:first-child { text-align: right; width: 40%; }
.ozet .sinifadi { display: flex; align-items: center; justify-content: flex-start; gap: 8px; }
.dogruluk { display: flex; gap: 10px; margin-bottom: 10px; }
.dogruluk div { flex: 1; text-align: center; border-radius: 8px; padding: 8px; border: 1px solid var(--line); }
.dogruluk b { display: block; font-size: 24px; }
.dogruluk span { color: var(--muted); font-size: 12px; }
.grafik { margin-top: 12px; }
.grafik svg { width: 100%; height: 90px; display: block; border-radius: 6px; cursor: pointer; }
.hamleler { margin-top: 12px; max-height: 360px; overflow: auto; position: relative; }
.hamleler .satir { display: grid; grid-template-columns: 38px 1fr 1fr; }
.hamleler .no { color: var(--muted); padding: 3px 4px; }
.hamleler .h { padding: 3px 6px; border-radius: 4px; cursor: pointer; display: flex; gap: 6px; align-items: center; }
.hamleler .h:hover { background: var(--sel); }
.hamleler .h.secili { background: var(--sel); font-weight: 700; }
.hamleler .h .simge { width: 16px; height: 16px; font-size: 9px; }
</style>
</head>
<body>
<header>
  __GERI__
  <h1>Oyun İncelemesi · <span id="baslik"></span></h1>
  <select id="oyunsec" aria-label="Oyun seç"></select>
  <button id="ses" aria-label="Ses"></button>
  <button id="cevir" title="Tahtayı çevir (F)">⇅ Çevir</button>
</header>
<main>
  <section>
    <div class="card">
      <div class="oyuncu" id="ust"></div>
      <div class="tahta-sar">
        <div class="evalbar" id="evalbar"><div class="beyaz" id="evalbeyaz"></div><div class="yazi" id="evalyazi"></div></div>
        <div class="tahta" id="tahta"></div>
      </div>
      <div class="oyuncu" id="alt"></div>
      <div class="kontrol">
        <button id="ilk" title="Başa (Home)">⏮</button>
        <button id="geri" title="Geri (←)">◀</button>
        <button id="ileri" title="İleri (→)">▶</button>
        <button id="son" title="Sona (End)">⏭</button>
      </div>
      <div class="aciklama" id="aciklama"></div>
    </div>
  </section>
  <section>
    <div class="card ozet">
      <div class="dogruluk" id="dogruluk"></div>
      <table id="ozet"></table>
      <div class="grafik"><svg id="grafik"></svg></div>
      <div class="hamleler" id="hamleler"></div>
    </div>
  </section>
</main>
<script>
const VERI = __VERI__;
const S = VERI.siniflar;
let oyun = null, ply = 0, cevrik = false, eniyiGoster = false;
const IYI = ["eniyi", "parlak", "harika", "zorunlu", "kitap"];
function benimAdim() { try { return (localStorage.getItem("mai-ad") || "").trim().toLowerCase(); } catch { return ""; } }

document.getElementById("baslik").textContent = __BASLIK__;
const sec = document.getElementById("oyunsec");
VERI.oyunlar.forEach((o, i) => {
  const op = document.createElement("option");
  op.value = i; op.textContent = `#${o.no} ${o.beyaz} – ${o.siyah} (${o.sonuc})`;
  sec.appendChild(op);
});
if (VERI.oyunlar.length < 2) sec.style.display = "none";
sec.onchange = () => yukle(+sec.value);

function simge(sinif, ek = "") {
  const [ad, s, renk] = S[sinif];
  return `<span class="simge ${ek}" style="background:${renk}" title="${ad}">${s}</span>`;
}
function esc(t) { return String(t).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c])); }

function yukle(i) {
  oyun = VERI.oyunlar[i]; ply = 0; eniyiGoster = false;
  const ad = benimAdim();
  cevrik = !!ad && oyun.siyah.toLowerCase() === ad;
  const d = oyun.dogruluk;
  document.getElementById("dogruluk").innerHTML =
    `<div><span>${esc(oyun.beyaz)}</span><b>${d.w ?? "–"}</b><span>doğruluk</span></div>` +
    `<div><span>${esc(oyun.siyah)}</span><b>${d.b ?? "–"}</b><span>doğruluk</span></div>`;
  let t = `<tr><th></th><th>Beyaz</th><th>Siyah</th></tr>`;
  for (const s of VERI.sira) {
    const w = oyun.sayac.w[s], b = oyun.sayac.b[s];
    t += `<tr><td><span class="sinifadi">${simge(s)} ${S[s][0]}</span></td>` +
         `<td style="color:${S[s][2]};font-weight:700">${w}</td><td style="color:${S[s][2]};font-weight:700">${b}</td></tr>`;
  }
  document.getElementById("ozet").innerHTML = t;
  hamleListesi(); grafik(); ciz();
}

function hamleListesi() {
  let h = "", satir = null;
  oyun.plylar.forEach((p, i) => {
    if (p.renk === "w" || i === 0) {
      if (satir !== null) h += satir + "</div>";
      satir = `<div class="satir"><span class="no">${p.no}.</span>`;
      if (p.renk === "b") satir += `<span class="h"></span>`;
    }
    satir += `<span class="h" data-i="${i + 1}">${simge(p.sinif)}${esc(p.san)}</span>`;
  });
  if (satir !== null) h += satir + "</div>";
  const el = document.getElementById("hamleler");
  el.innerHTML = h;
  el.querySelectorAll(".h[data-i]").forEach(x => x.onclick = () => git(+x.dataset.i));
}

function grafik() {
  const g = document.getElementById("grafik");
  const n = oyun.plylar.length, W = Math.max(200, g.clientWidth || 600), H = 90;
  g.setAttribute("viewBox", `0 0 ${W} ${H}`);
  const ys = [oyun.baslangic_kazanma, ...oyun.plylar.map(p => p.kazanma_beyaz)];
  const x = i => n ? (i / n) * W : 0, y = k => H - k * H;
  let yol = `M0 ${H} L0 ${y(ys[0])}`;
  ys.forEach((k, i) => yol += ` L${x(i)} ${y(k)}`);
  yol += ` L${W} ${H} Z`;
  let noktalar = "";
  oyun.plylar.forEach((p, i) => {
    if (["parlak", "harika", "hata", "kacirma", "buyukhata"].includes(p.sinif))
      noktalar += `<circle cx="${x(i + 1)}" cy="${y(p.kazanma_beyaz)}" r="4" fill="${S[p.sinif][2]}" stroke="#fff" stroke-width="1"/>`;
  });
  g.innerHTML = `<rect width="${W}" height="${H}" fill="#403d39"/><path d="${yol}" fill="#f0efec"/>` +
    `<line x1="0" x2="${W}" y1="${H / 2}" y2="${H / 2}" stroke="#888" stroke-dasharray="6 6"/>` +
    `<line id="imlec" y1="0" y2="${H}" stroke="#81b64c" stroke-width="2"/>` + noktalar;
  g.onclick = e => {
    const r = g.getBoundingClientRect();
    git(Math.round(((e.clientX - r.left) / r.width) * n));
  };
}

function kareXY(kare) {
  const s = kare.charCodeAt(0) - 97, r = +kare[1] - 1;
  return cevrik ? [7 - s, r] : [s, 7 - r];
}

function ciz(anim) {
  const p = ply > 0 ? oyun.plylar[ply - 1] : null;
  const deneme = eniyiGoster && p && p.eniyi_fen;
  const fen = deneme ? p.eniyi_fen : (p ? p.fen : oyun.baslangic_fen);
  const satirlar = fen.split(" ")[0].split("/");
  const tahta = [];
  satirlar.forEach(s => { const r = []; for (const c of s) { if (/\d/.test(c)) for (let k = 0; k < +c; k++) r.push(null); else r.push(c); } tahta.push(r); });
  const gosterilen = deneme ? p.eniyi_uci : (p ? p.uci : null);
  const vurgu = gosterilen ? [gosterilen.slice(0, 2), gosterilen.slice(2, 4)] : [];
  const rozetSinif = deneme ? "eniyi" : (p ? p.sinif : null);
  let h = "";
  for (let gy = 0; gy < 8; gy++) for (let gx = 0; gx < 8; gx++) {
    const r = cevrik ? gy : 7 - gy, s = cevrik ? 7 - gx : gx;
    const ad = String.fromCharCode(97 + s) + (r + 1);
    const tas = tahta[7 - r][s];
    const acik = (r + s) % 2 === 1;
    let ic = tas ? `<img src="${VERI.taslar[tas]}" alt="${tas}">` : "";
    if (gx === 0) ic += `<span class="koord sira">${r + 1}</span>`;
    if (gy === 7) ic += `<span class="koord sutun">${String.fromCharCode(97 + s)}</span>`;
    if (rozetSinif && ad === vurgu[1]) ic += `<span class="rozet" style="background:${S[rozetSinif][2]}">${S[rozetSinif][1]}</span>`;
    h += `<div class="kare ${acik ? "a" : "k"}${vurgu.includes(ad) ? " vurgu" : ""}" data-k="${ad}">${ic}</div>`;
  }
  // En iyi hamle oku (oynanan hamle en iyi değilse)
  if (!deneme && p && p.eniyi_uci && p.eniyi_uci !== p.uci && !IYI.includes(p.sinif)) {
    const [x1, y1] = kareXY(p.eniyi_uci.slice(0, 2)), [x2, y2] = kareXY(p.eniyi_uci.slice(2, 4));
    const cx1 = x1 + .5, cy1 = y1 + .5, cx2 = x2 + .5, cy2 = y2 + .5;
    const dx = cx2 - cx1, dy = cy2 - cy1, L = Math.hypot(dx, dy), ux = dx / L, uy = dy / L;
    const ex = cx2 - ux * .3, ey = cy2 - uy * .3;
    h += `<svg class="oklar" viewBox="0 0 8 8"><defs><marker id="uc" markerWidth="3" markerHeight="3" refX="1.2" refY="1.5" orient="auto">` +
         `<path d="M0,0 L3,1.5 L0,3 Z" fill="var(--arrow)"/></marker></defs>` +
         `<line x1="${cx1}" y1="${cy1}" x2="${ex}" y2="${ey}" stroke="var(--arrow)" stroke-width=".2" stroke-linecap="round" marker-end="url(#uc)"/></svg>`;
  }
  document.getElementById("tahta").innerHTML = h;
  animasyon(anim);

  const ust = cevrik ? oyun.beyaz : oyun.siyah, alt = cevrik ? oyun.siyah : oyun.beyaz;
  document.getElementById("ust").textContent = ust;
  document.getElementById("alt").textContent = alt;

  const kz = p ? p.kazanma_beyaz : oyun.baslangic_kazanma;
  const skor = p ? p.skor : oyun.baslangic_skor;
  const beyaz = document.getElementById("evalbeyaz"), yazi = document.getElementById("evalyazi");
  beyaz.style.height = (cevrik ? 100 - kz * 100 : kz * 100) + "%";
  if (cevrik) { beyaz.style.top = "0"; beyaz.style.bottom = "auto"; } else { beyaz.style.bottom = "0"; beyaz.style.top = "auto"; }
  yazi.textContent = skor.replace("+", "");
  const beyazOnde = kz >= 0.5;
  const altta = beyazOnde !== cevrik;
  yazi.style.top = altta ? "auto" : "3px"; yazi.style.bottom = altta ? "3px" : "auto";
  yazi.style.color = beyazOnde ? "#403d39" : "#f0efec";

  const ac = document.getElementById("aciklama");
  if (!p) ac.innerHTML = `<div class="bas">Başlangıç pozisyonu</div><div class="pv">Değerlendirme: ${esc(oyun.baslangic_skor)}</div>`;
  else {
    const [ad] = S[p.sinif];
    const nokta = p.renk === "w" ? "." : "...";
    let m = `<div class="bas">${simge(p.sinif)} ${p.no}${nokta}${esc(p.san)} — ${ad}</div>`;
    const iyi = IYI.includes(p.sinif);
    if (deneme) m = `<div class="bas">${simge("eniyi")} ${p.no}${nokta}${esc(p.eniyi_san)} — Motorun en iyi hamlesi</div>` +
                    `<div class="pv">Oynanan: ${esc(p.san)} (${S[p.sinif][0]}) · en iyi devamın değerlendirmesi ${esc(p.eniyi_skor)}</div>`;
    else if (!iyi && p.eniyi_san) m += `<div class="pv">En iyisi <b>${esc(p.eniyi_san)}</b> (${esc(p.eniyi_skor)}) · kazanma şansı kaybı %${p.kayip}</div>`;
    if (p.sinif === "harika" && p.ikinci) m += `<div class="pv">Tek iyi hamle. İkinci en iyi: ${esc(p.ikinci)}</div>`;
    if (p.sinif === "parlak") m += `<div class="pv">Taş kurbanı ile en iyi devam.</div>`;
    if (p.pv.length) m += `<div class="pv">Motor devamı: ${esc(p.pv.join(" "))}</div>`;
    if (!deneme) m += `<div class="pv">Değerlendirme: ${esc(p.skor)} · hamle doğruluğu ${p.dogruluk}</div>`;
    if (!iyi && p.eniyi_fen)
      m += `<button class="eniyi-dugme" id="eniyiDugme">${deneme ? "↩ Oynanan hamleye dön" : "★ En iyi hamleyi göster"}</button>`;
    ac.innerHTML = m;
    const d = document.getElementById("eniyiDugme");
    if (d) d.onclick = eniyiDegistir;
  }

  document.querySelectorAll(".hamleler .h").forEach(x => x.classList.toggle("secili", +x.dataset.i === ply));
  const s = document.querySelector(".hamleler .h.secili");
  if (s) {
    const liste = document.getElementById("hamleler");
    const ust = s.offsetTop - liste.offsetTop, alt = ust + s.offsetHeight;
    if (ust < liste.scrollTop) liste.scrollTop = ust - 8;
    else if (alt > liste.scrollTop + liste.clientHeight) liste.scrollTop = alt - liste.clientHeight + 8;
  }
  const imlec = document.getElementById("imlec");
  const n = oyun.plylar.length;
  if (imlec) { const xx = n ? (ply / n) * (+document.getElementById("grafik").viewBox.baseVal.width || 0) : 0; imlec.setAttribute("x1", xx); imlec.setAttribute("x2", xx); }
}

function git(i) {
  const yeni = Math.max(0, Math.min(oyun.plylar.length, i));
  if (yeni === ply) return;
  let anim = null;
  if (yeni === ply + 1) anim = { uci: oyun.plylar[yeni - 1].uci, geri: false };
  else if (yeni === ply - 1) anim = { uci: oyun.plylar[ply - 1].uci, geri: true };
  const p = oyun.plylar[yeni - 1];
  ply = yeni;
  eniyiGoster = false;
  ciz(anim);
  if (anim && !anim.geri) sesCal(p.san);
  else sesCal(null);
}

function eniyiDegistir() {
  const p = ply > 0 ? oyun.plylar[ply - 1] : null;
  if (!p || !p.eniyi_fen || IYI.includes(p.sinif)) return;
  eniyiGoster = !eniyiGoster;
  // Önceki pozisyondan oynanmış gibi canlandır
  ciz({ uci: eniyiGoster ? p.eniyi_uci : p.uci, geri: false });
  sesCal(eniyiGoster ? p.eniyi_san : p.san);
}

// --- Ses: Web Audio ile üretilen tahta sesleri (dosya yok) ---
let sesAcik = true, ses = null, ana = null, sonSes = 0;
try { sesAcik = localStorage.getItem("mai-ses") !== "0"; } catch {}
function sesHazirla() {
  ses = new (window.AudioContext || window.webkitAudioContext)();
  ana = ses.createGain(); ana.gain.value = 0.55;
  const sikistir = ses.createDynamicsCompressor();
  ana.connect(sikistir).connect(ses.destination);
}
// Ahşap tahtaya konan taş: kısa, alçak bir "tok" — gövde (sinüs) + çok az sürtünme (alçak geçiren gürültü)
function tok(t0, frekans, guc) {
  const o = ses.createOscillator(), g = ses.createGain();
  o.type = "sine";
  o.frequency.setValueAtTime(frekans * 1.6, t0);
  o.frequency.exponentialRampToValueAtTime(frekans, t0 + 0.03);
  g.gain.setValueAtTime(0.0001, t0);
  g.gain.exponentialRampToValueAtTime(guc, t0 + 0.004);
  g.gain.exponentialRampToValueAtTime(0.0001, t0 + 0.11);
  o.connect(g).connect(ana); o.start(t0); o.stop(t0 + 0.12);

  const n = Math.floor(ses.sampleRate * 0.03), buf = ses.createBuffer(1, n, ses.sampleRate), d = buf.getChannelData(0);
  for (let k = 0; k < n; k++) d[k] = (Math.random() * 2 - 1) * Math.pow(1 - k / n, 4);
  const kaynak = ses.createBufferSource(); kaynak.buffer = buf;
  const filtre = ses.createBiquadFilter(); filtre.type = "lowpass"; filtre.frequency.value = 900;
  const gg = ses.createGain(); gg.gain.value = guc * 0.35;
  kaynak.connect(filtre).connect(gg).connect(ana); kaynak.start(t0);
}
function cin(t0, frekans, sure, guc) {
  const o = ses.createOscillator(), g = ses.createGain();
  o.type = "triangle"; o.frequency.value = frekans;
  g.gain.setValueAtTime(0.0001, t0);
  g.gain.exponentialRampToValueAtTime(guc, t0 + 0.02);
  g.gain.exponentialRampToValueAtTime(0.0001, t0 + sure);
  o.connect(g).connect(ana); o.start(t0); o.stop(t0 + sure + 0.02);
}
function sesCal(san) {
  if (!sesAcik) return;
  try {
    if (!ses) sesHazirla();
    if (ses.state === "suspended") ses.resume();
  } catch { return; }
  // Hızlı geçişte sesler üst üste binmesin: çok sık basılırsa kısık çal
  const simdi = performance.now(), sik = simdi - sonSes < 90;
  sonSes = simdi;
  const k = sik ? 0.35 : 1;
  const t = ses.currentTime + 0.005;
  if (san === null) { tok(t, 260, 0.25 * k); return; }
  if (san.includes("#")) { tok(t, 180, 0.6 * k); if (!sik) { cin(t + 0.08, 392, 0.7, 0.06); cin(t + 0.08, 494, 0.8, 0.05); } return; }
  if (san.startsWith("O-O")) { tok(t, 220, 0.45 * k); tok(t + 0.09, 240, 0.35 * k); }
  else if (san.includes("x")) tok(t, 170, 0.65 * k);
  else tok(t, 220, 0.45 * k);
  if (san.includes("+") && !sik) cin(t + 0.02, 587, 0.25, 0.035);
}
function sesDugmesi() {
  const d = document.getElementById("ses");
  d.textContent = sesAcik ? "🔊" : "🔇";
  d.title = sesAcik ? "Sesi kapat (M)" : "Sesi aç (M)";
}
function sesDegistir() {
  sesAcik = !sesAcik;
  try { localStorage.setItem("mai-ses", sesAcik ? "1" : "0"); } catch {}
  sesDugmesi();
}

// --- Animasyon: taşı eski karesinden yenisine kaydır ---
function kaydir(kimden, kime) {
  const el = document.querySelector(`.kare[data-k="${kime}"] img`);
  if (!el) return;
  const [x1, y1] = kareXY(kimden), [x2, y2] = kareXY(kime);
  el.style.transition = "none";
  el.style.zIndex = 4;
  el.style.transform = `translate(${(x1 - x2) * 100}%, ${(y1 - y2) * 100}%)`;
  el.getBoundingClientRect();
  el.style.transition = "transform .16s ease-out";
  el.style.transform = "";
  el.addEventListener("transitionend", () => { el.style.zIndex = ""; }, { once: true });
}
function animasyon(anim) {
  if (!anim || matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const a = anim.uci.slice(0, 2), b = anim.uci.slice(2, 4);
  const cifter = [];
  // Rok: şahla birlikte kaleyi de kaydır
  const roklar = { e1g1: ["h1", "f1"], e1c1: ["a1", "d1"], e8g8: ["h8", "f8"], e8c8: ["a8", "d8"] };
  if (roklar[anim.uci]) {
    const tas = document.querySelector(`.kare[data-k="${anim.geri ? roklar[anim.uci][0] : roklar[anim.uci][1]}"] img`);
    if (tas && /[Rr]/.test(tas.alt)) cifter.push(roklar[anim.uci]);
  }
  cifter.unshift([a, b]);
  for (const [k1, k2] of cifter) anim.geri ? kaydir(k2, k1) : kaydir(k1, k2);
}
document.getElementById("ilk").onclick = () => git(0);
document.getElementById("geri").onclick = () => git(ply - 1);
document.getElementById("ileri").onclick = () => git(ply + 1);
document.getElementById("son").onclick = () => git(oyun.plylar.length);
document.getElementById("cevir").onclick = () => { cevrik = !cevrik; ciz(); };
document.getElementById("ses").onclick = sesDegistir;
sesDugmesi();
document.addEventListener("keydown", e => {
  if (e.target.tagName === "SELECT") return;
  if (e.key === "ArrowLeft") git(ply - 1);
  else if (e.key === "ArrowRight") git(ply + 1);
  else if (e.key === "Home") git(0);
  else if (e.key === "End") git(oyun.plylar.length);
  else if (e.key === "f" || e.key === "F") { cevrik = !cevrik; ciz(); }
  else if (e.key === "m" || e.key === "M") sesDegistir();
  else if (e.key === "b" || e.key === "B") eniyiDegistir();
  else return;
  e.preventDefault();
});
let boyutZaman;
window.addEventListener("resize", () => { clearTimeout(boyutZaman); boyutZaman = setTimeout(() => { grafik(); ciz(); }, 150); });
yukle(0);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
