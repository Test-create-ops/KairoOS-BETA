# KairoOS — Windows App (.exe + Microsoft Store)

KairoOS si distribuisce su Windows in due modi:

| File | Dove | Scopo |
|---|---|---|
| `KairoOS-Setup.exe` | GitHub Releases | Installer (NSIS): installa KairoOS + QEMU + ISO, scorciatoia desktop |
| `KairoOS-x86_64.msix` | Microsoft Store | Pacchetto che Microsoft firma e ospita gratis |

KairoOS gira dentro **QEMU** (emulatore, licenza GPL): la finestra che vedi è un PC virtuale
che esegue l'ISO `viteza.iso`. Non serve Windows nativo nel senso tradizionale — funziona
su qualsiasi PC x86-64.

## Requisiti sul Mac

```bash
brew install mingw-w64 p7zip makensis
# + l'SDK msix-packaging compilato (solo per il pacchetto Store):
#   git clone https://github.com/microsoft/msix-packaging && cd msix-packaging
#   cmake -DMACOS=on -DMSIX_PACK=on -DUSE_MSIX_SDK_ZLIB=on -DXML_PARSER=xerces \
#         -DUSE_VALIDATION_PARSER=on -DCMAKE_BUILD_TYPE=Release -B out
#   cmake --build out --target PackSample
```

## Build

```bash
make iso                      # genera viteza.iso (se non già fatto)
cd tools/winapp
PACK_SAMPLE=/percorso/PackSample ./build-winapp.sh
```

Risultati in `tools/winapp/out/`:
- `KairoOS-Setup.exe` — installer per GitHub
- `KairoOS-x86_64.msix` — pacchetto per lo Store
- `payload/` — cartella finale (launcher + qemu + viteza.iso)

Lo script scarica automaticamente QEMU per Windows (~197 MB) da qemu.weilnetz.de e
impacchetta solo l'essenziale: `qemu-system-x86_64.exe`, le DLL, il firmware UEFI
`edk2-x86_64-code.fd` e l'ISO.

Opzioni: `--local-qemu DIR` (usa binari già scaricati), `--skip-msix`, `--skip-nsis`.

## Prima della pubblicazione: identità Partner Center

1. Apri **storedeveloper.microsoft.com**, registra l'account **Individual** (gratis,
   verifica con documento + selfie).
2. In Partner Center: **Apps & games → New product → MSIX/PWA** → riserva il nome
   **"KairoOS"** (valido 3 mesi).
3. Nell'overview del prodotto: **Product management → View app identity details**.
   Ti darà i valori esatti (Publisher, ecc.).
4. Sostituisci i placeholder in `tools/winapp/AppxManifest.xml`:

   ```xml
   <Identity Name="KairoOS"
             Publisher="CN=IL_TUO_PUBLISHER_REALE"
             Version="1.0.0.0" />
   <Properties>
     <PublisherDisplayName>IL_TUO_NOME</PublisherDisplayName>
   </Properties>
   ```

5. Rilanci `build-winapp.sh`: il `.msix` ora ha la tua identità.

> Nota: `Name` in `<Identity>` NON è il nome riservato dello Store (può anche
> differire), ma il `Publisher` deve corrispondere a quello dell'account.

## Invio allo Store

Nel prodotto riservato: **Start Submission**, poi compila le schede:

1. **Pricing & availability** — Gratis, tutti i mercati
2. **Properties** — Categoria: *Developer Tools / Utilities*; requisiti: x64, Windows 10 1809+
3. **Age ratings** — IARC questionnaire (nessun contenuto violento: app educativa/utility)
4. **Packages** — carica `KairoOS-x86_64.msix` (NON serve firma: la mette Microsoft)
5. **Store listing** — descrizione, screenshots del desktop KairoOS, logo
6. **Submission options** — note per la certificazione: spiega che è un emulatore QEMU
   che avvia un OS hobby x86-64

La certificazione richiede fino a 3 giorni lavorativi. Quando pubblica, il pacchetto
viene risignato e ospitato sul CDN Microsoft.

## Licenza QEMU (GPL)

QEMU è coperto da GPLv2: il pacchetto include `COPYING`/`COPYING.LIB` e il file `VERSION`.
Per la conformità completa vai su https://www.qemu.org e indica come ottenere il sorgente
(nel listing dello Store / README di GitHub).

## File

- `kairoos_launcher.c` — launcher Win32 (GUI dark, Avvia/Stop, a schermo intero)
- `kairoos.rc` — icona + info versione dell'exe
- `AppxManifest.xml` — manifest MSIX (placeholder da sostituire)
- `gen_assets.py` — genera icone PNG/ICO (nessuna dipendenza)
- `kairoos.nsi` — installer NSIS (GitHub)
- `build-winapp.sh` — pipeline completa (Mac)
