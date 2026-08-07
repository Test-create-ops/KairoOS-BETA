#!/usr/bin/env python3
"""Proxy per Viteza OS — collega seriale kernel a servizi esterni (AI, API)."""
import socket, json, urllib.request, sys, time, subprocess, platform, threading, signal, os, struct, wave, tempfile

import stt_engine

OLLAMA_URL = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "llama3.2"

# ─── Wi-Fi scan (CoreWLAN via Swift, nessun sudo) ──────────────────────────
WIFI_SWIFT_SRC = os.path.join(tempfile.gettempdir(), "viteza_wifiscan.swift")
WIFI_SWIFT_SRC_TEXT = (
    "import CoreWLAN\n"
    "let client = CWWiFiClient.shared()\n"
    "if let interface = client.interface() {\n"
    "    do {\n"
    "        let networks = try interface.scanForNetworks(withName: nil, includeHidden: true)\n"
    "        for n in networks {\n"
    "            var name = \"(hidden)\"\n"
    "            if let sd = n.ssidData {\n"
    "                if let s = String(data: sd, encoding: .utf8), !s.isEmpty { name = s }\n"
    "            }\n"
    "            print(name + \"|\" + String(n.rssiValue) + \"|SEC\")\n"
    "        }\n"
    "        if networks.isEmpty { print(\"EMPTY\") }\n"
    "    } catch {\n"
    "        print(\"ERR:\" + String(describing: error))\n"
    "    }\n"
    "} else {\n"
    "    print(\"NOIFACE\")\n"
    "}\n"
)


def ensure_wifi_swift():
    if not os.path.exists(WIFI_SWIFT_SRC):
        with open(WIFI_SWIFT_SRC, "w") as fh:
            fh.write(WIFI_SWIFT_SRC_TEXT)


def wifi_scan(f):
    """Scansiona le reti Wi-Fi reali dell'host (CoreWLAN) e le invia al kernel."""
    ensure_wifi_swift()
    try:
        out = subprocess.run(["swift", WIFI_SWIFT_SRC],
                             capture_output=True, timeout=30).stdout.decode(errors="replace")
    except Exception as e:
        print(f"[proxy] Wi-Fi scan error: {e}", file=sys.stderr)
        f.write("WIFI|NO SCAN|0|1\n")
        f.write("END\n")
        f.flush()
        return
    sent = 0
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith("PASS"):
            continue
        parts = line.split("|")
        if len(parts) == 3:
            safe = parts[0].replace("|", "/")[:39]
            f.write(f"WIFI|{safe}|{parts[1]}|{parts[2]}\n")
            f.flush()
            sent += 1
    if sent == 0:
        f.write("WIFI|(no networks found)|0|1\n")
        f.flush()
    f.write("END\n")
    f.flush()
    print(f"[proxy] Wi-Fi scan: {sent} networks", file=sys.stderr)

# ─── Speech-to-text (motore custom MFCC+DTW) ──────────────────────────────
stt_proc = None        # processo ffmpeg di registrazione
stt_pcm = None         # file pcm in scrittura
stt_train_phrase = None
stt_lock = threading.Lock()

STT_APPLE_APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stt_apple.app")


def pcm_to_wav(pcm_path, wav_path, rate=stt_engine.SAMPLE_RATE):
    with wave.open(wav_path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        with open(pcm_path, 'rb') as p:
            w.writeframes(p.read())


def apple_transcribe(pcm_path, timeout=90):
    """Dettatura libera con Apple Speech (on-device). Ritorna (testo, None) o (None, errore)."""
    wav = pcm_path + ".wav"
    out = pcm_path + ".out"
    try:
        os.unlink(wav)
        os.unlink(out)
    except Exception:
        pass
    try:
        pcm_to_wav(pcm_path, wav)
    except Exception as e:
        return None, f"wav conversion: {e}"
    try:
        subprocess.run(
            ["open", "-n", STT_APPLE_APP, "--args", wav, "it-IT", out],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    except Exception as e:
        return None, f"stt_apple launch: {e}"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(out):
            try:
                text = open(out, encoding="utf-8").read().strip()
            finally:
                os.unlink(out)
            if text.startswith("ERR:"):
                return None, text[4:].strip()
            return text, None
        time.sleep(0.5)
    return None, "nessun output da stt_apple"


STT_TEST_PCM = os.environ.get("STT_TEST_PCM", "")


def stt_start(train_phrase=None):
    global stt_proc, stt_pcm, stt_train_phrase
    stt_stop(kill=True)
    stt_train_phrase = train_phrase
    if STT_TEST_PCM and os.path.exists(STT_TEST_PCM):
        stt_pcm = STT_TEST_PCM
        stt_proc = None
        print(f"[proxy] STT: modalità TEST, uso {STT_TEST_PCM}", file=sys.stderr)
        return
    stt_pcm = tempfile.mktemp(suffix=".pcm")
    dev = getattr(stt_engine, "MIC_DEVICE", ":0")
    stt_proc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-f", "avfoundation", "-i", dev,
         "-ac", "1", "-ar", str(stt_engine.SAMPLE_RATE),
         "-f", "s16le", "-y", stt_pcm],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"[proxy] STT: registrazione avviata (train={train_phrase})", file=sys.stderr)


def stt_stop(kill=False):
    global stt_proc, stt_pcm
    if stt_proc:
        try:
            if not kill:
                stt_proc.terminate()
                try:
                    stt_proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    stt_proc.kill(); stt_proc.wait()
            else:
                stt_proc.kill(); stt_proc.wait()
        except Exception:
            pass
        stt_proc = None


def stt_finalize(f):
    """Ferma la registrazione e invia il PCM (mono 16k) al kernel per il
    riconoscimento in-kernel (DTW nativo). Il kernel risponderà con STT|AUDIO
    solo se vuole il fallback Apple Speech."""
    global stt_pcm, stt_train_phrase
    stt_stop(kill=False)
    time.sleep(0.3)
    path = stt_pcm
    stt_pcm = None
    stt_train_phrase = None

    if STT_TEST_PCM and path == STT_TEST_PCM and os.path.exists(path):
        print("[proxy] STT: finalize in modalità TEST (file non cancellato)", file=sys.stderr)
    elif not path or not os.path.exists(path) or os.path.getsize(path) < 4000:
        if path and os.path.exists(path):
            os.unlink(path)
        try:
            f.buffer.write(b"STT|PCM|0\n")
            f.buffer.flush()
        except Exception:
            pass
        print("[proxy] STT: nessun audio registrato", file=sys.stderr)
        return

    try:
        with open(path, 'rb') as p:
            data = p.read()
    except Exception:
        data = b""
    finally:
        try:
            if path != STT_TEST_PCM:
                os.unlink(path)
        except Exception:
            pass
    if len(data) > 160000:
        data = data[:160000]
    nsamples = len(data) // 2
    vals = struct.unpack('<%dh' % nsamples, data[:nsamples * 2])
    ssum = sum(vals)
    print(f"[proxy] STT: inviati {nsamples} campioni, sum={ssum}", file=sys.stderr)
    try:
        f.buffer.write(f"STT|PCM|{nsamples}\n".encode())
        f.buffer.write(data)
        f.buffer.flush()
    except Exception as e:
        print(f"[proxy] STT invio PCM error: {e}", file=sys.stderr)
        return
    print(f"[proxy] STT: inviati {nsamples} campioni al kernel", file=sys.stderr)

def stt_audio(f, rate, channels, nsamples):
    """Riceve audio PCM s16le (mono) via seriale e lo trascrive con Apple Speech."""
    global stt_train_phrase
    stt_stop(kill=True)
    stt_train_phrase = None
    nbytes = nsamples * 2
    buf = b""
    try:
        while len(buf) < nbytes:
            chunk = f.buffer.read(nbytes - len(buf))
            if not chunk:
                break
            buf += chunk
    except Exception as e:
        print(f"[proxy] STT audio read error: {e}", file=sys.stderr)
    path = tempfile.mktemp(suffix=".pcm")
    with open(path, "wb") as p:
        p.write(buf)
    try:
        with open("/tmp/e2e_rx.pcm", "wb") as dbg:
            dbg.write(buf)
    except Exception:
        pass
    if len(buf) < 4000:
        print(f"[proxy] STT audio troppo corto ({len(buf)} byte)", file=sys.stderr)
        try:
            os.unlink(path)
        except Exception:
            pass
        f.write("RESP|[Nessun audio registrato]\n"); f.flush()
        f.write("END\n"); f.flush()
        return
    try:
        sig = stt_engine.load_raw_pcm(path, sr=rate)
    except Exception as e:
        print(f"[proxy] STT load pcm: {e}", file=sys.stderr)
        sig = None
    text, err = apple_transcribe(path)
    if not text:
        print(f"[proxy] STT: Apple Speech: {err}; fallback DTW", file=sys.stderr)
        if sig is not None:
            phrase, conf = stt_engine.recognize_signal(sig)
            print(f"[proxy] STT: riconosciuto '{phrase}' (conf {conf:.2f})", file=sys.stderr)
            text = phrase if phrase else "[Non ho capito. Riprova o allena una frase nuova]"
        else:
            text = "[Non ho capito. Riprova o allena una frase nuova]"
    safe = text.replace('|', '/').strip()[:180]
    print(f"[proxy] STT: dettato '{safe}'", file=sys.stderr)
    f.write(f"RESP|{safe}\n"); f.flush()
    try:
        os.unlink(path)
    except Exception:
        pass
    f.write("END\n"); f.flush()

def kill_tts():
    try:
        plat = platform.system()
        if plat == "Darwin":
            subprocess.run(["pkill", "-f", "say -r"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif plat == "Linux":
            subprocess.run(["pkill", "-f", "espeak"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except:
        pass

def speak(text):
    try:
        plat = platform.system()
        if plat == "Darwin":
            subprocess.Popen(["say", "-r", "180", text], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif plat == "Linux":
            subprocess.Popen(["espeak", text], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif plat == "Windows":
            import pyttsx3
            pyttsx3.speak(text)
    except:
        pass

def ollama_chat(prompt):
    try:
        data = json.dumps({"model": OLLAMA_MODEL, "prompt": prompt, "stream": False}).encode()
        req = urllib.request.Request(OLLAMA_URL, data=data, headers={"Content-Type": "application/json"})
        resp = urllib.request.urlopen(req, timeout=30)
        result = json.loads(resp.read())
        return result.get("response", "").strip()
    except Exception as e:
        print(f"[proxy] Ollama error: {e}", file=sys.stderr)
        return None

class SerialLineReader:
    """Buffered binary reader over the serial socket.
    readline() returns decoded text (errors=replace); read(n) returns raw bytes."""

    def __init__(self, sock, chunk=65536):
        self.sock = sock
        self.chunk = chunk
        self.buf = b""

    def _fill(self):
        data = self.sock.recv(self.chunk)
        if not data:
            return False
        self.buf += data
        return True

    def readline(self):
        while b"\n" not in self.buf:
            if not self._fill():
                break
        if b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            return line.decode("utf-8", errors="replace").strip()
        line, self.buf = self.buf, b""
        return line.decode("utf-8", errors="replace").strip()

    def read(self, n):
        while len(self.buf) < n:
            if not self._fill():
                break
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def write(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.sock.sendall(data)

    def flush(self):
        pass


def main():
    HOST, PORT = "localhost", 9000
    retry_delay = 2
    while True:
        try:
            s = socket.socket()
            s.settimeout(2.0)
            s.connect((HOST, PORT))
            s.settimeout(None)
            print(f"[proxy] Connected to QEMU serial", file=sys.stderr)
            retry_delay = 2
            f = SerialLineReader(s)
            while True:
                line = f.readline()
                if not line:
                    break
                cmd = line.strip()
                if cmd:
                    print(f"[proxy] RX: {cmd[:60]}", file=sys.stderr)
                if cmd.startswith("STT|"):
                    arg = cmd[4:]
                    if arg == "START":
                        stt_start()
                    elif arg.startswith("TRAIN|"):
                        stt_start(train_phrase=arg[6:])
                    elif arg.startswith("AUDIO|"):
                        meta = arg[6:].split('|')
                        try:
                            rate = int(meta[0]); channels = int(meta[1]); ns = int(meta[2])
                        except Exception:
                            rate, channels, ns = 16000, 1, 0
                        stt_audio(f, rate, channels, ns)
                    elif arg == "STOP":
                        stt_finalize(f)
                    elif arg == "ABORT":
                        stt_stop(kill=True)
                        print("[proxy] STT: registrazione annullata", file=sys.stderr)
                elif cmd.startswith("AI|"):
                    prompt = cmd[3:]
                    print(f"[proxy] AI query: {prompt[:60]}...", file=sys.stderr)
                    resp = ollama_chat(prompt)
                    if resp:
                        print(f"[proxy] RESP: {resp[:60]}...", file=sys.stderr)
                        for rline in resp.split('\n'):
                            if rline.strip():
                                safe = rline.replace('|', '/').strip()[:180]
                                f.write(f"RESP|{safe}\n")
                                f.flush()
                        threading.Thread(target=speak, args=(resp,), daemon=True).start()
                    else:
                        f.write(f"RESP|[Ollama not available. Install: brew install ollama && ollama pull {OLLAMA_MODEL}]\n")
                        f.flush()
                    f.write("END\n")
                    f.flush()
                elif cmd.startswith("RUN|"):
                    rest = cmd[4:]
                    sep = rest.find('|')
                    if sep > 0:
                        lang = rest[:sep]
                        code = rest[sep+1:]
                    else:
                        lang = "code"
                        code = rest
                    print(f"[proxy] Kairo Studio RUN ({lang}): {code[:60]}...", file=sys.stderr)
                    prompt = f"You are a code interpreter. Execute the following {lang} code and show ONLY the output or any errors. No explanations, no extra text:\n\n{code}"
                    resp = ollama_chat(prompt)
                    if resp:
                        for rline in resp.split('\n'):
                            if rline.strip():
                                safe = rline.replace('|', '/').strip()[:180]
                                f.write(f"RESP|{safe}\n")
                                f.flush()
                    else:
                        f.write(f"RESP|[Ollama not available]\n")
                        f.flush()
                    f.write("END\n")
                    f.flush()
                elif cmd.startswith("WEB|"):
                    url = cmd[4:].strip()
                    print(f"[proxy] Web fetch: {url}", file=sys.stderr)
                    try:
                        req = urllib.request.Request(url, headers={"User-Agent": "VitezaOS/1.0"})
                        resp = urllib.request.urlopen(req, timeout=15)
                        html = resp.read().decode("utf-8", errors="replace")
                        # Strip HTML tags, return plain text
                        import re
                        text = re.sub(r'<script[^>]*>.*?</script>', '', html, flags=re.DOTALL)
                        text = re.sub(r'<style[^>]*>.*?</style>', '', text, flags=re.DOTALL)
                        text = re.sub(r'<[^>]+>', ' ', text)
                        text = re.sub(r'\s+', ' ', text).strip()
                        if len(text) > 2000:
                            text = text[:2000] + "..."
                        for rline in text.split('\n'):
                            if rline.strip():
                                safe = rline.replace('|', '/').strip()[:180]
                                f.write(f"RESP|{safe}\n")
                                f.flush()
                    except Exception as e:
                        print(f"[proxy] Web error: {e}", file=sys.stderr)
                        f.write(f"RESP|[Error: {str(e)[:60]}]\n")
                        f.flush()
                    f.write("END\n")
                    f.flush()
                elif cmd.startswith("WIFI|"):
                    if cmd.strip() == "WIFI|SCAN":
                        wifi_scan(f)
                    else:
                        f.write("WIFI|BAD CMD|0|1\n")
                        f.write("END\n")
                        f.flush()
                elif cmd.startswith("WTHR|"):
                    city = cmd[5:].strip()
                    print(f"[proxy] Weather request: {city}", file=sys.stderr)
                    COORDS = {
                        "San+Francisco": (37.7749, -122.4194),
                        "New+York": (40.7128, -74.0060),
                        "Tokyo": (35.6762, 139.6503),
                        "London": (51.5074, -0.1278),
                        "Dubai": (25.2048, 55.2708),
                        "Sydney": (-33.8688, 151.2093),
                    }
                    try:
                        lat, lon = COORDS.get(city, (0,0))
                        url = f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code&timezone=auto"
                        import urllib.request, json
                        resp = json.loads(urllib.request.urlopen(url, timeout=10).read())
                        cur = resp["current"]
                        temp = int(cur["temperature_2m"] + 0.5)
                        wc = cur["weather_code"]
                        if wc in (51,53,55,56,57,61,63,65,66,67,71,73,75,77,80,81,82,85,86,95,96,99):
                            c = 2
                        elif wc in (1,2,3,45,48):
                            c = 1
                        else:
                            c = 0
                        f.write(f"RESP|{temp}|{c}\n")
                    except Exception as e:
                        print(f"[proxy] Weather error: {e}", file=sys.stderr)
                        f.write("RESP|--|0\n")
                    f.write("END\n")
                    f.flush()
            s.close()
        except ConnectionRefusedError:
            print(f"[proxy] QEMU not ready, retry in {retry_delay}s...", file=sys.stderr)
            time.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 30)
        except Exception as e:
            print(f"[proxy] Error: {e}, reconnect in 5s...", file=sys.stderr)
            time.sleep(5)
        finally:
            kill_tts()

if __name__ == "__main__":
    main()
