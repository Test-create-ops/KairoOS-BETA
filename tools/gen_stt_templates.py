#!/usr/bin/env python3
"""Rigenera da zero tutti i template STT (10 frasi x 8 voci TTS)."""
import os
import sys
import glob
import shutil

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import stt_engine

VOICES = "Alice,Eddy,Flo,Grandma,Reed,Rocko,Sandy,Shelley"
PHRASES = [
    "ciao oreo",
    "come stai",
    "bene grazie",
    "aiutami",
    "apri terminal",
    "apri la calcolatrice",
    "che ore sono",
    "grazie mille",
    "arrivederci",
    "dimmi qualcosa di interessante",
]

tdir = stt_engine.TEMPLATE_DIR
if os.path.isdir(tdir):
    shutil.rmtree(tdir)
os.makedirs(tdir, exist_ok=True)

with open(stt_engine.VOCAB_FILE, "w") as f:
    f.write("{}\n")

for phrase in PHRASES:
    voices = [v.strip() for v in VOICES.split(",") if v.strip()]
    made = stt_engine.generate_templates(phrase, voices)
    print(f"'{phrase}': {len(made)} voci")

print("fatto:", len(glob.glob(os.path.join(tdir, "*.npy"))), "template")
