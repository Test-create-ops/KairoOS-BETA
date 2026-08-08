# WindiroOS — Requisiti di sistema per lo Store

Guida passo-passo per compilare i requisiti di sistema nella submission su
Microsoft Partner Center. WindiroOS è un emulatore (QEMU) che avvia un OS
hobby x86-64: **non richiede GPU dedicata**, il rendering è software.

## Requisiti reali del prodotto

| Voce | Minimo | Raccomandato |
|---|---|---|
| Processore | 1 GHz x64 | 2 core x64, 2.5 GHz |
| RAM | 2 GB | 4 GB |
| GPU | Nessuna (rendering software) | Nessuna (rendering software) |
| DirectX | Non richiesto | Non richiesto |
| Windows | 10 1809+ / 11 | 10 1809+ / 11 |

## Dove si compilano i campi

Ci sono **due punti** nel modulo di submission:

1. **Properties → System requirements** → solo checkbox, lasciale tutte VUOTE.
2. **Store listing → Additional system requirements** → qui si incollano le
   voci (fino a 11 per colonna, max 200 caratteri per voce).

### 1) Properties → System requirements (checkbox)

Spuntare **solo** "Physical keyboard and mouse" nella colonna **Minimum**.
Tutte le altre voci restano vuote, in entrambe le colonne:

| Voce | Minimum | Recommended |
|---|---|---|
| Physical keyboard and mouse | ✅ Sì | ❌ No |
| Touchscreen | ❌ No | ❌ No |
| Microphone | ❌ No | ❌ No |
| Xbox controller or gamepad | ❌ No | ❌ No |
| Windows Mixed Reality motion controllers | ❌ No | ❌ No |
| Windows Mixed Reality immersive headset | ❌ No | ❌ No |

Motivo: WindiroOS si usa solo con tastiera e mouse, quindi sono "minimum
hardware". Spuntare voci in "recommended" implica che l'hardware sia un
vantaggio/opzione — qui non lo è, e spunte superflue fanno comparire
avvisi o escludono il pacchetto dalle collezioni giuste.

### 2) Store listing → Additional system requirements (testo da incollare)

**Minimum hardware**

```
Processor: 1 GHz x64
Memory: 2 GB RAM
Graphics: none required (software rendering)
```

**Recommended hardware**

```
Processor: 2-core x64, 2.5 GHz
Memory: 4 GB RAM
Graphics: none required (software rendering)
```

Niente bullet manuali: lo Store li aggiunge da solo.

## Note per la certificazione (Submission options)

Nel campo note della submission, aggiungi una spiegazione in inglese:

> "WindiroOS is an emulator app that boots a hobby x86-64 operating system
> inside QEMU. It uses software rendering (standard VGA); no DirectX or
> dedicated GPU is required. Minimum guest RAM is 256 MB, 512 MB recommended."

Questo evita che la certificazione pensi a un'app grafica pesante o blocchi
il pacchetto per "richieste hardware non dichiarate".

## Testo bonus per la descrizione dello Store (listing)

> **System requirements:** works on any x64 PC with 2 GB RAM (4 GB
> recommended). No graphics card, no DirectX, no touchscreen required.
> WindiroOS runs inside QEMU with software rendering.

## Checklist finale

- [ ] Properties → System requirements: solo "Physical keyboard and mouse" in Minimum; tutte le altre vuote (anche in Recommended)
- [ ] Store listing → Minimum hardware: processore / RAM / grafica incollati
- [ ] Store listing → Recommended hardware: processore / RAM / grafica incollati
- [ ] Submission notes: spiegazione emulatore + software rendering in inglese
- [ ] Nessun requisito DirectX/GPU dichiarato da nessuna parte
