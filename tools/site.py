#!/usr/bin/env python3
"""MaiEngine analiz sitesi — PGN'i bırak, oyun incelemesini al.

Kullanım:
    tools/.venv/bin/python tools/site.py            # http://localhost:8080
    tools/.venv/bin/python tools/site.py --port 9000 --threads 8

Analizler sırayla tek bir motor sürecinde yapılır; raporlar analizler/ klasörüne
de HTML olarak kaydedilir.
"""

import argparse
import io
import json
import os
import queue
import re
import threading
import time
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import chess.engine
import chess.pgn
import chess.polyglot

import analiz

KAYIT_DIZINI = os.path.join(analiz.KOK, "analizler")
MAKS_OYUN = 20
MODLAR = {"hizli": 0.2, "normal": 0.5, "derin": 1.5}

isler = {}            # id -> iş durumu
sira = queue.Queue()  # bekleyen iş id'leri
kilit = threading.Lock()


def pgn_oyunlari(metin):
    oyunlar = []
    f = io.StringIO(metin)
    while len(oyunlar) < MAKS_OYUN:
        oyun = chess.pgn.read_game(f)
        if oyun is None:
            break
        if oyun.errors:
            raise ValueError(f"PGN okunamadı: {oyun.errors[0]}")
        if oyun.end().ply() > oyun.ply():
            oyunlar.append(oyun)
    return oyunlar


def dosya_adi(o):
    ad = f"{o['beyaz']}-vs-{o['siyah']}-{time.strftime('%Y%m%d-%H%M%S')}"
    return re.sub(r"[^\w.-]+", "_", ad)[:120] + ".analiz.html"


def isci(ayarlar):
    analizci = None
    kitap = None
    if os.path.exists(analiz.VARSAYILAN_KITAP):
        kitap = chess.polyglot.open_reader(analiz.VARSAYILAN_KITAP)
    while True:
        is_id = sira.get()
        i = isler[is_id]
        try:
            if analizci is None:
                analizci = analiz.Analizci(ayarlar.motor, chess.engine.Limit(time=0.5),
                                           ayarlar.threads, ayarlar.hash)
            analizci.limit = chess.engine.Limit(time=MODLAR[i["mod"]])
            i["durum"] = "calisiyor"
            sonuclar = []
            for no, oyun in enumerate(i["oyunlar"], 1):
                i["oyun_no"] = no

                def ilerleme(k, n, i=i):
                    i["konum"], i["toplam"] = k + 1, n

                o = analiz.oyunu_analiz_et(oyun, analizci, False, ilerleme, kitap)
                o["no"] = no
                sonuclar.append(o)
            baslik = f"{sonuclar[0]['beyaz']} – {sonuclar[0]['siyah']}"
            i["html"] = analiz.html_uret(sonuclar, baslik, geri="/")
            i["ozet"] = {
                "beyaz": sonuclar[0]["beyaz"], "siyah": sonuclar[0]["siyah"],
                "sonuc": sonuclar[0]["sonuc"], "dogruluk": sonuclar[0]["dogruluk"],
                "oyun_sayisi": len(sonuclar), "zaman": time.time(),
            }
            os.makedirs(KAYIT_DIZINI, exist_ok=True)
            with open(os.path.join(KAYIT_DIZINI, dosya_adi(sonuclar[0])), "w", encoding="utf-8") as f:
                f.write(i["html"])
            i["durum"] = "bitti"
        except Exception as e:  # motor çöktüyse bir sonraki işte yeniden başlat
            i["durum"], i["hata"] = "hata", str(e)
            if analizci is not None:
                try:
                    analizci.kapat()
                except Exception:
                    pass
                analizci = None
        finally:
            i.pop("oyunlar", None)


class Isleyici(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def gonder(self, kod, govde, tur="text/html; charset=utf-8"):
        veri = govde.encode() if isinstance(govde, str) else govde
        self.send_response(kod)
        self.send_header("Content-Type", tur)
        self.send_header("Content-Length", str(len(veri)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(veri)

    def json_gonder(self, kod, nesne):
        self.gonder(kod, json.dumps(nesne, ensure_ascii=False), "application/json; charset=utf-8")

    def do_GET(self):
        yol = self.path.split("?")[0]
        if yol == "/":
            self.gonder(200, ANA_SAYFA)
        elif yol == "/gecmis":
            with kilit:
                liste = [{"id": k, **v["ozet"]} for k, v in isler.items() if v["durum"] == "bitti"]
            liste.sort(key=lambda x: -x["zaman"])
            self.json_gonder(200, liste[:12])
        elif yol.startswith("/durum/"):
            i = isler.get(yol[7:])
            if not i:
                return self.json_gonder(404, {"hata": "iş yok"})
            bekleyen = list(sira.queue)
            self.json_gonder(200, {
                "durum": i["durum"], "hata": i.get("hata"),
                "konum": i.get("konum", 0), "toplam": i.get("toplam", 0),
                "oyun_no": i.get("oyun_no", 0), "oyun_sayisi": i["oyun_sayisi"],
                "sirada": bekleyen.index(yol[7:]) + 1 if yol[7:] in bekleyen else 0,
            })
        elif yol.startswith("/rapor/"):
            i = isler.get(yol[7:])
            if not i or i["durum"] != "bitti":
                return self.gonder(404, "Rapor bulunamadı")
            self.gonder(200, i["html"])
        else:
            self.gonder(404, "Bulunamadı")

    def do_POST(self):
        if self.path != "/analiz":
            return self.gonder(404, "Bulunamadı")
        uzunluk = int(self.headers.get("Content-Length", 0))
        if uzunluk > 5_000_000:
            return self.json_gonder(413, {"hata": "Dosya çok büyük"})
        try:
            istek = json.loads(self.rfile.read(uzunluk))
            mod = istek.get("mod", "normal")
            if mod not in MODLAR:
                mod = "normal"
            oyunlar = pgn_oyunlari(istek.get("pgn", ""))
        except Exception as e:
            return self.json_gonder(400, {"hata": str(e)})
        if not oyunlar:
            return self.json_gonder(400, {"hata": "PGN'de hamlesi olan bir oyun bulunamadı."})
        is_id = uuid.uuid4().hex[:10]
        with kilit:
            isler[is_id] = {"durum": "sirada", "mod": mod, "oyunlar": oyunlar, "oyun_sayisi": len(oyunlar)}
        sira.put(is_id)
        self.json_gonder(200, {"id": is_id})


ANA_SAYFA = r"""<!doctype html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MaiEngine Analiz</title>
<style>
:root {
  --bg: #f6f4ef; --panel: #fffdf9; --text: #1f1d1a; --muted: #77716a; --line: #e4dfd6;
  --accent: #5f7d43; --accent-ink: #ffffff; --soft: #eef0e6; --danger: #b4432f;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #1f1d1b; --panel: #282624; --text: #ece8e1; --muted: #9b958c; --line: #3a3733;
    --accent: #8fb26b; --accent-ink: #1b1a17; --soft: #2f332a; --danger: #e07a64;
  }
}
:root[data-theme="dark"] {
  --bg: #1f1d1b; --panel: #282624; --text: #ece8e1; --muted: #9b958c; --line: #3a3733;
  --accent: #8fb26b; --accent-ink: #1b1a17; --soft: #2f332a; --danger: #e07a64;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
  font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
.kap { max-width: 640px; margin: 0 auto; padding: 56px 16px 48px; }
.marka { display: flex; align-items: center; gap: 12px; margin-bottom: 6px; }
.logo { width: 34px; height: 34px; display: grid; grid-template-columns: repeat(4, 1fr); border-radius: 6px; overflow: hidden; flex: none; }
.logo i { background: #ebecd0; } .logo i.k { background: #739552; }
h1 { font-family: "Iowan Old Style", "Palatino Linotype", Georgia, serif; font-weight: 600; font-size: 34px;
  letter-spacing: -.01em; margin: 0; }
.alt { color: var(--muted); margin: 0 0 32px; }
.kart { background: var(--panel); border: 1px solid var(--line); border-radius: 14px; padding: 20px; }
.birak { border: 1.5px dashed var(--line); border-radius: 10px; padding: 26px 16px; text-align: center;
  cursor: pointer; transition: border-color .15s, background .15s; }
.birak:hover, .birak.uzerinde { border-color: var(--accent); background: var(--soft); }
.birak b { display: block; font-size: 16px; margin-bottom: 2px; }
.birak span { color: var(--muted); font-size: 13px; }
.birak.dolu b { color: var(--accent); }
.ayrac { display: flex; align-items: center; gap: 12px; color: var(--muted); font-size: 12px; margin: 16px 0; }
.ayrac::before, .ayrac::after { content: ""; flex: 1; height: 1px; background: var(--line); }
textarea { width: 100%; min-height: 120px; resize: vertical; border: 1px solid var(--line); border-radius: 10px;
  background: var(--bg); color: var(--text); padding: 12px; font: 13px/1.5 ui-monospace, Menlo, monospace; }
textarea:focus { outline: 2px solid var(--accent); outline-offset: -1px; border-color: transparent; }
.adim { display: flex; align-items: center; gap: 10px; margin-top: 12px; font-size: 13px; color: var(--muted); }
.adim span { white-space: nowrap; }
.adim input { flex: 1; min-width: 0; border: 1px solid var(--line); border-radius: 8px; background: var(--bg); color: var(--text);
  padding: 8px 10px; font: inherit; font-size: 13px; }
.adim input:focus { outline: 2px solid var(--accent); outline-offset: -1px; border-color: transparent; }
.alt-satir { display: flex; gap: 12px; align-items: center; justify-content: space-between; margin-top: 16px; flex-wrap: wrap; }
.mod { display: inline-flex; border: 1px solid var(--line); border-radius: 9px; padding: 3px; background: var(--bg); }
.mod label { position: relative; }
.mod input { position: absolute; opacity: 0; }
.mod span { display: block; padding: 6px 12px; border-radius: 6px; cursor: pointer; font-size: 13px; color: var(--muted); }
.mod input:checked + span { background: var(--panel); color: var(--text); font-weight: 600; box-shadow: 0 1px 2px rgba(0,0,0,.08); }
.mod input:focus-visible + span { outline: 2px solid var(--accent); }
button.ana { background: var(--accent); color: var(--accent-ink); border: 0; border-radius: 9px; padding: 10px 20px;
  font: 600 15px/1 inherit; font-family: inherit; cursor: pointer; }
button.ana:disabled { opacity: .5; cursor: default; }
.ipucu { color: var(--muted); font-size: 12px; margin-top: 10px; }
.hata { color: var(--danger); font-size: 14px; margin-top: 12px; }
.ilerleme { display: none; }
.ilerleme .baslik { font-weight: 600; margin-bottom: 4px; }
.ilerleme .detay { color: var(--muted); font-size: 13px; margin-bottom: 14px; }
.cubuk { height: 8px; background: var(--soft); border-radius: 4px; overflow: hidden; }
.cubuk div { height: 100%; width: 0; background: var(--accent); transition: width .3s; }
.gecmis { margin-top: 36px; }
.gecmis h2 { font-size: 13px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: .06em; margin: 0 0 10px; }
.gecmis a { display: flex; justify-content: space-between; gap: 12px; padding: 12px 14px; border: 1px solid var(--line);
  border-radius: 10px; background: var(--panel); color: var(--text); text-decoration: none; margin-bottom: 8px; }
.gecmis a:hover { border-color: var(--accent); }
.gecmis .sag { color: var(--muted); font-size: 13px; white-space: nowrap; }
footer { color: var(--muted); font-size: 12px; text-align: center; margin-top: 40px; }
</style>
</head>
<body>
<div class="kap">
  <div class="marka">
    <div class="logo" aria-hidden="true">
      <i></i><i class="k"></i><i></i><i class="k"></i><i class="k"></i><i></i><i class="k"></i><i></i>
      <i></i><i class="k"></i><i></i><i class="k"></i><i class="k"></i><i></i><i class="k"></i><i></i>
    </div>
    <h1>MaiEngine Analiz</h1>
  </div>
  <p class="alt">Oyununu bırak, her hamlesini motor gözüyle değerlendirelim. Yumuşatma yok.</p>

  <div class="kart" id="form">
    <div class="birak" id="birak" tabindex="0" role="button" aria-label="PGN dosyası seç">
      <b id="birakBaslik">PGN dosyasını buraya bırak</b>
      <span id="birakAlt">ya da seçmek için tıkla</span>
    </div>
    <input type="file" id="dosya" accept=".pgn,text/plain" hidden>
    <div class="ayrac">veya yapıştır</div>
    <textarea id="pgn" placeholder="1. e4 e5 2. Nf3 Nc6 3. Bb5 …" spellcheck="false"></textarea>
    <label class="adim">
      <span>Kullanıcı adın</span>
      <input id="ad" type="text" placeholder="isteğe bağlı · siyahla oynadıysan tahta senin tarafından açılır" autocomplete="off" spellcheck="false">
    </label>
    <div class="alt-satir">
      <div class="mod" role="radiogroup" aria-label="Analiz derinliği">
        <label><input type="radio" name="mod" value="hizli"><span>Hızlı</span></label>
        <label><input type="radio" name="mod" value="normal" checked><span>Normal</span></label>
        <label><input type="radio" name="mod" value="derin"><span>Derin</span></label>
      </div>
      <button class="ana" id="basla">Analiz et</button>
    </div>
    <div class="ipucu" id="ipucu">Normal: hamle başına yaklaşık 1 saniye. 40 hamlelik bir oyun ~1,5 dakika sürer.</div>
    <div class="hata" id="hata" role="alert"></div>
  </div>

  <div class="kart ilerleme" id="ilerleme" aria-live="polite">
    <div class="baslik" id="ilBaslik">Analiz ediliyor…</div>
    <div class="detay" id="ilDetay">Motor hazırlanıyor</div>
    <div class="cubuk"><div id="ilCubuk"></div></div>
  </div>

  <div class="gecmis" id="gecmis" hidden>
    <h2>Son analizler</h2>
    <div id="gecmisListe"></div>
  </div>
  <footer>MaiEngine v3 · Stockfish 19 arama ve NNUE</footer>
</div>
<script>
const $ = id => document.getElementById(id);
const IPUCU = {
  hizli: "Hızlı: hamle başına yaklaşık 0,4 saniye. Kabaca bakmak için.",
  normal: "Normal: hamle başına yaklaşık 1 saniye. 40 hamlelik bir oyun ~1,5 dakika sürer.",
  derin: "Derin: hamle başına yaklaşık 3 saniye. 40 hamlelik bir oyun ~4–5 dakika sürer."
};
try { $("ad").value = localStorage.getItem("mai-ad") || ""; } catch {}
$("ad").oninput = () => { try { localStorage.setItem("mai-ad", $("ad").value.trim()); } catch {} };
document.querySelectorAll('input[name=mod]').forEach(r => r.onchange = () => $("ipucu").textContent = IPUCU[r.value]);

function dosyaOku(f) {
  if (!f) return;
  const r = new FileReader();
  r.onload = () => {
    $("pgn").value = r.result;
    $("birak").classList.add("dolu");
    $("birakBaslik").textContent = f.name;
    $("birakAlt").textContent = "yüklendi · değiştirmek için tekrar bırak";
  };
  r.readAsText(f);
}
const birak = $("birak");
birak.onclick = () => $("dosya").click();
birak.onkeydown = e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); $("dosya").click(); } };
$("dosya").onchange = e => dosyaOku(e.target.files[0]);
["dragenter", "dragover"].forEach(t => birak.addEventListener(t, e => { e.preventDefault(); birak.classList.add("uzerinde"); }));
["dragleave", "drop"].forEach(t => birak.addEventListener(t, e => { e.preventDefault(); birak.classList.remove("uzerinde"); }));
birak.addEventListener("drop", e => dosyaOku(e.dataTransfer.files[0]));
document.addEventListener("dragover", e => e.preventDefault());
document.addEventListener("drop", e => { e.preventDefault(); if (e.target !== birak) dosyaOku(e.dataTransfer.files[0]); });

$("basla").onclick = async () => {
  const pgn = $("pgn").value.trim();
  $("hata").textContent = "";
  if (!pgn) { $("hata").textContent = "Önce bir PGN bırak ya da yapıştır."; return; }
  $("basla").disabled = true;
  const mod = document.querySelector('input[name=mod]:checked').value;
  try {
    const c = await fetch("/analiz", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ pgn, mod }) });
    const j = await c.json();
    if (!c.ok) throw new Error(j.hata || "Bir şeyler ters gitti");
    $("form").style.display = "none";
    $("ilerleme").style.display = "block";
    takip(j.id);
  } catch (e) {
    $("hata").textContent = e.message;
    $("basla").disabled = false;
  }
};

async function takip(id) {
  const basla = Date.now();
  while (true) {
    let d;
    try { d = await (await fetch("/durum/" + id)).json(); } catch { await bekle(1000); continue; }
    if (d.durum === "bitti") { location.href = "/rapor/" + id; return; }
    if (d.durum === "hata") {
      $("ilerleme").style.display = "none"; $("form").style.display = "block";
      $("basla").disabled = false; $("hata").textContent = "Analiz başarısız: " + d.hata; return;
    }
    if (d.durum === "sirada") {
      $("ilDetay").textContent = d.sirada > 1 ? `Sırada ${d.sirada - 1} analiz var, bekleniyor` : "Motor hazırlanıyor";
    } else if (d.toplam) {
      const oran = d.konum / d.toplam;
      const genel = ((d.oyun_no - 1) + oran) / d.oyun_sayisi;
      $("ilCubuk").style.width = (genel * 100).toFixed(1) + "%";
      const oyunMetni = d.oyun_sayisi > 1 ? `Oyun ${d.oyun_no}/${d.oyun_sayisi} · ` : "";
      let kalan = "";
      const gecen = (Date.now() - basla) / 1000;
      if (genel > 0.05) { const s = Math.max(0, Math.round(gecen / genel - gecen)); kalan = ` · ~${s < 60 ? s + " sn" : Math.ceil(s / 60) + " dk"} kaldı`; }
      const asama = d.konum * 2 > d.toplam + 1 ? "Hamleler sınıflandırılıyor" : "Pozisyonlar analiz ediliyor";
      $("ilDetay").textContent = `${oyunMetni}${asama} · %${Math.round(genel * 100)}${kalan}`;
    }
    await bekle(700);
  }
}
const bekle = ms => new Promise(r => setTimeout(r, ms));
function esc(t) { return String(t).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c])); }

fetch("/gecmis").then(r => r.json()).then(l => {
  if (!l.length) return;
  $("gecmis").hidden = false;
  $("gecmisListe").innerHTML = l.map(o =>
    `<a href="/rapor/${o.id}"><span>${esc(o.beyaz)} – ${esc(o.siyah)}${o.oyun_sayisi > 1 ? ` <small>(+${o.oyun_sayisi - 1} oyun)</small>` : ""}</span>` +
    `<span class="sag">${esc(o.sonuc)} · ${o.dogruluk.w ?? "–"} / ${o.dogruluk.b ?? "–"}</span></a>`).join("");
}).catch(() => {});
</script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description="MaiEngine analiz sitesi")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--motor", default=analiz.VARSAYILAN_MOTOR)
    ap.add_argument("--threads", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--hash", type=int, default=256)
    ap.add_argument("--acma", action="store_true", help="tarayıcıyı otomatik açma")
    a = ap.parse_args()
    if not os.path.exists(a.motor):
        raise SystemExit(f"Motor bulunamadı: {a.motor}  (önce 'make' ile derle)")

    threading.Thread(target=isci, args=(a,), daemon=True).start()
    sunucu = ThreadingHTTPServer(("127.0.0.1", a.port), Isleyici)
    adres = f"http://localhost:{a.port}"
    print(f"MaiEngine Analiz: {adres}  (kapatmak için Ctrl+C)")
    if not a.acma:
        threading.Timer(0.5, webbrowser.open, args=(adres,)).start()
    try:
        sunucu.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
