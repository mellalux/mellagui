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
- **H.264 täiendused** (`src/h264enc.c`): liikumisotsing (kerimine annaks palju väiksema faili), deblocking ja B-kaadrid. Salvestaja kasutab praegu x264 kui ffmpeg on olemas, nii et see on peamiselt ffmpeg-ita masinatele.
- **Enda rakendused:** süsteemimonitor graafikutega, muusikamängija, tekstiredaktori süntaksivärvimine, lihtne pildiredaktor.
- **Mitme monitori parandused:** kui väljund eemaldatakse, jäävad tavalised aknad vana koordinaadi peale, mis võib olla nähtamatu (fullscreen ja maximized aknad järgivad juba väljundit, vt `lm_wm_output_added`).

## Soovitus

1. Idle-inhibit ja toitemenüü: väikesed ja parandavad igapäevast kasutust.
2. Salvestaja heli ja kursor: salvestaja saab valmis tööriistaks.
3. VNC-server, kui tahad midagi uut.
