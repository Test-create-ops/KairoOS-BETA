#!/usr/bin/env python3
"""Proxy per Viteza OS — collega seriale kernel a servizi esterni (AI, API)."""
import socket, json, urllib.request, sys, time, subprocess, platform, threading, signal

OLLAMA_URL = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "llama3.2"

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

def main():
    HOST, PORT = "localhost", 9000
    retry_delay = 2
    while True:
        try:
            s = socket.socket()
            s.settimeout(None)
            s.connect((HOST, PORT))
            print(f"[proxy] Connected to QEMU serial", file=sys.stderr)
            retry_delay = 2
            f = s.makefile("rw", buffering=1)
            while True:
                line = f.readline()
                if not line:
                    break
                cmd = line.strip()
                if cmd.startswith("AI|"):
                    prompt = cmd[3:]
                    print(f"[proxy] AI query: {prompt[:60]}...", file=sys.stderr)
                    resp = ollama_chat(prompt)
                    if resp:
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
