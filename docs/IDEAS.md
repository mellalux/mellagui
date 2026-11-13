# Ideed: mida MellaGUI-ga veel ehitada

Nimekiri sellest, mis sobiks sellele, mis meil juba on. Töömaht on umbkaudne.

## Väiksed, aga kasulikud (1–2 tundi)

- ~~**Idle-inhibit protokoll**~~ — tehtud (`src/idleinhibit.c`): screensaver ja automaatne lukustus ootavad, kuni nähtav aken seda palub; viewer kasutab seda video mängimisel.
- **Toitemenüü:** sessiooni dialoogis on ainult Lock ja Exit. Lisada Suspend, Restart ja Shutdown. Meil on juba oma D-Bus klient (`src/logind.c`), seega on need vaid mõned kutsed.
- **Salvestaja kursor:** praegu kursorit pildis pole. Kompositor teab hiire asukohta, nii et selle saaks kaadrisse joonistada.
- **Tray parem klõps:** praegu saab tray-kirjel ainult klõpsata. Menüü (nt "Stop", "Pause") teeks salvestajast tõelise ülemise riba tööriista.
- **Kalender kella klõpsust:** klõps ülariba kellal avab väikese kalendri.

## Keskmised (päev)

- **Salvestaja heli:** PulseAudio/PipeWire sisend ja süsteemiheli koos videoga. Lihtsaim viis on ffmpeg-i toru, enda helikodeerija on suurem töö.
- **Piirkonna või akna salvestus** ja **ekraanipildi valik hiirega** (ala valida, kommenteerida).
- **Võrgu- ja Bluetooth-leht** seadetes, NetworkManagerile D-Bus kaudu.
- **Märguannete ajalugu**, et mööduvaid kaarte saaks hiljem vaadata.
- **Tray näitajad:** aku, Wi-Fi ja helitugevus ribale, kasutades sama tray-liidest (`mellagui_tray_v1`).
- **Protokollid välisrakendustele:** `wlr-screencopy` (grim, OBS), `xdg-output`, `xdg-activation`, `pointer-constraints`.
- **Faili- ja viewer'i täiendused:** pisipildid, otsing, arhiivid; PDF-vaataja.

## Suuremad ja põnevamad

- **VNC/kaugtöölaud:** meil on juba ekraanipildi liides (`mellagui_capture_v1`), H.264 kodeerija ja sisendi süstimine (`lm_input_inject`). Neist saaks ehitada serveri, millega vaadata ja juhtida seda arvutit teisest (sobib KVM-i olukorraga).
- **HiDPI ja murdskaleerimine:** väljundi skaala on praegu alati 1.
- **Aknahaldus:** tiling, aknaeelvaated Alt+Tab-is, töölaudade ülevaade, animatsioonid ja akna reeglid (rakendus avaneb kindlas kohas).
- ~~**H.264 täiendused**~~ — liikumisotsing ja deblocking on tehtud (`src/h264enc.c`, `src/h264.c`; kerimiskaader on umbes 30–50 korda väiksem, ffmpeg dekodeerib baidi-baidilt samamoodi); B-kaadreid ei tehtud (ümberjärjestus, ekraanisisu puhul vähe tulu).
- **Enda rakendused:** süsteemimonitor graafikutega, muusikamängija, tekstiredaktori süntaksivärvimine, lihtne pildiredaktor.
- **Mitme monitori parandused:** kui väljund eemaldatakse, jäävad tavalised aknad vana koordinaadi peale, mis võib olla nähtamatu (fullscreen ja maximized aknad järgivad juba väljundit, vt `lm_wm_output_added`).

## Soovitus

1. Idle-inhibit ja toitemenüü: väikesed ja parandavad igapäevast kasutust.
2. Salvestaja heli ja kursor: salvestaja saab valmis tööriistaks.
3. VNC-server, kui tahad midagi uut.

---

## Uus nimekiri (2026-11-13)
Nimekiri on koostatud 2026-11-13 projekti seisu põhjal. Märge **[tehtud]** / **[pooleli]** näitab, kus ollakse.

## Kasulikud ja lähedal sellele, mida just tegime

- **Päikesepõhised reeglid nutikodus** **[tehtud]** - rulood avanevad päikesetõusul ja sulguvad loojangul. `mellagui-smart --background`
  täidab juba ajareegleid; lisandub trigger "päikesetõus / loojang ± N min" (`client/smartsun.[ch]`, `client/smartsched.[ch]`). Asukoht tuleb
  ajavööndist (`zone1970.tab`) või käsust `mellagui-smart --place LAT LON`.
- **Salvestaja kiirus ja kvaliteet** - täispikslites 16x16 liikumisotsing **[tehtud]**, deblocking **[tehtud]**. Jäänud: kvartpikslid või 8x8
  plokid (väike võit), bitikiiruse reguleerimine (kindel faili suurus pika salvestise jaoks). B-kaadreid ei soovitata (ümberjärjestus, ekraanisisu
  puhul vähe tulu).
- **Ekraanijagamine** - teised programmid ei näe praegu MellaGUI ekraani (on ainult privaatne `mellagui_capture_v1`). PipeWire-põhine portal teeks
  videokõned ja brauseri ekraanijagamise võimalikuks.

## Midagi uut

- **Silmaga juhitud ekraan** - arendusmasinas on Tobii EyeChip. Saver tuhmuks, kui vaadata mujale; lukk käivituks, kui kaamera enam kasutajat ei näe;
  fookus võiks järgida pilku. Kõige omapärasem, aga ka kõige ebakindlam: pole teada, kas Linuxis saab sõltumatut andmevoogu.
- **Meediakontroll ja MPRIS** - muusika nupud kiirseadete kaardil ja lukustuskuval ("praegu mängib"). Lukustuskuval on praegu ainult kell ja PIN.
- **Tapeedi slaidiseanss ja päevaaja järgi vahetuv tapeet** - sobib hästi lukustuskuva udustatud taustaga.
- **Aknapaigutused** - kiire ruudustik ja snap-asetused (tiled layouts), serva lohistamine. Praegu on ülevaade ja töölauad, aga mitte lõpuni välja
  arendatud paigutusi.

## Mitte ehitamine, aga vajalik

- **Proovida päris ekraanil** - fade, lipud, lukustuskuva tagatapeet ja öövalgus on nähtud ainult peata (headless) režiimis, mitte päris DRM/GL-i
  peal. Kontrollnimekiri on `docs/HARDWARE.md`. Tasub teha enne uusi suuri asju.

## Soovitus

Päikesepõhised reeglid esimesena (väike, kasutab olemasolevat, tulemuse näeb oma rullooga). Suuremast: ekraanijagamine.
