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
