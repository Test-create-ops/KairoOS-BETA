#!/bin/bash
# Viteza Discord Publisher
# Uso: ./discord-publish.sh [webhook_url]

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

WEBHOOK="$1"
if [ -z "$WEBHOOK" ]; then
    if [ -f .discord_webhook ]; then
        WEBHOOK=$(cat .discord_webhook)
    else
        echo "Nessun webhook configurato."
        echo "Uso: $0 https://discord.com/api/webhooks/..."
        exit 1
    fi
fi

echo ">>> Building kernel..."
make 2>&1 | tail -3

echo ">>> Building ISO..."
bash iso/mkiso.sh 2>&1 | tail -2

echo ">>> Updating Viteza.app..."
APP="$DIR/dist/Viteza.app/Contents"
cp kernel.bin "$APP/Resources/kernel.bin"
cp viteza.iso "$APP/Resources/viteza.iso"
cp social_proxy.py "$APP/Resources/social_proxy.py"
chmod +x "$APP/MacOS/launcher"

VERSION=$(date +%Y%m%d)

WEATHER=$(python3 -c "
import urllib.request, json
cities = {'San Francisco':(37.7749,-122.4194),'New York':(40.7128,-74.0060),'Tokyo':(35.6762,139.6503),'London':(51.5074,-0.1278),'Dubai':(25.2048,55.2708),'Sydney':(-33.8688,151.2093)}
out=[]
for name,(lat,lon) in cities.items():
    url=f'https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code&timezone=auto'
    d=json.loads(urllib.request.urlopen(url,timeout=10).read())
    t=int(d['current']['temperature_2m']+0.5)
    wc=d['current']['weather_code']
    sym='☀️' if wc==0 else '⛅' if wc<=3 else '🌧️' if wc>=51 else '☁️'
    out.append(f'{sym} {name}: {t}°C')
print(' | '.join(out))
" 2>/dev/null)

TMPFILE=$(mktemp)
cat > "$TMPFILE" << EOF
📦 Viteza OS — Build $VERSION

🌤 $WEATHER

(what's new)
EOF

echo ">>> Apro editor per il messaggio..."
${EDITOR:-nano} "$TMPFILE"
CHANGELOG=$(cat "$TMPFILE")
rm "$TMPFILE"

echo ">>> Packaging..."
ZIPNAME="Viteza-v$VERSION.zip"
rm -f "$DIR/$ZIPNAME"

cd "$DIR/dist"
zip -r "$DIR/$ZIPNAME" Viteza.app viteza.iso social_proxy.py run_macos.command run_windows.bat run_linux.sh 2>&1 | tail -1
cd "$DIR"

echo ">>> Publishing..."
JSON_SAFE=$(echo "$CHANGELOG" | python3 -c 'import sys,json; print(json.dumps({"content":sys.stdin.read()}))')
curl -s -F "file=@$ZIPNAME" \
     -F "payload_json=$JSON_SAFE" \
     "$WEBHOOK" && echo "✅ Pubblicato su #updates"
