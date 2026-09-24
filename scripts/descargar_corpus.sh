#!/bin/bash
# Descarga el Top 100 EBooks de los últimos 30 días de Project Gutenberg
# desde el espejo configurado. Los resultados requieren verificación.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROYECTO_DIR="$(dirname -- "$SCRIPT_DIR")"
CORPUS_DIR="$PROYECTO_DIR/corpus"
LISTADO_HTML="$SCRIPT_DIR/top100.html"
IDS_TXT="$SCRIPT_DIR/ids_30dias.txt"
LOG="$SCRIPT_DIR/descarga.log"
MIRROR="https://aleph.pglaf.org"
mkdir -p "$CORPUS_DIR"
: > "$LOG"
echo "Descargando la página del top 100..."
wget -q --timeout=15 --tries=2 -O "$LISTADO_HTML" "https://www.gutenberg.org/browse/scores/top"
echo "Extrayendo IDs de la sección 'last 30 days'..."
awk '/Top 100 EBooks last 30 days/{flag=1} /Top 100 Authors last 30 days/{flag=0} flag' "$LISTADO_HTML" \
  | grep -oE '/ebooks/[0-9]+' \
  | grep -oE '[0-9]+' \
  | awk '!seen[$0]++' > "$IDS_TXT"
TOTAL_IDS=$(wc -l < "$IDS_TXT")
echo "IDs encontrados: $TOTAL_IDS"
if [ "$TOTAL_IDS" -ne 100 ]; then
  echo "ERROR: se esperaban 100 IDs y se encontraron $TOTAL_IDS." | tee -a "$LOG"
  exit 1
fi
OK=0
FALLIDOS=0
CONTADOR=0
INICIO=$(date +%s)
echo "Se han encontrado $TOTAL_IDS IDs. Iniciando descargas, esto puede tardar varios minutos, por favor espere..."
while read -r id; do
  DEST="$CORPUS_DIR/${id}.txt"
  URL="$MIRROR/cache/epub/${id}/pg${id}.txt"
  if wget -q --timeout=15 --tries=2 -O "$DEST" "$URL" && [ -s "$DEST" ]; then
    OK=$((OK+1))
    echo "OK  $id" >> "$LOG"
  else
    rm -f "$DEST"
    FALLIDOS=$((FALLIDOS+1))
    echo "FALLA $id (sin texto plano, no disponible, o tiempo de espera agotado)" >> "$LOG"
  fi
  CONTADOR=$((CONTADOR+1))
  if [ $((CONTADOR % 5)) -eq 0 ]; then
    TRANSCURRIDO=$(( $(date +%s) - INICIO ))
    echo "Descargando... ($CONTADOR/$TOTAL_IDS, ${TRANSCURRIDO}s transcurridos)"
  fi
  sleep 1
done < "$IDS_TXT"
echo "Descarga completa. Exitosos: $OK  Solicitudes fallidas: $FALLIDOS"
echo "Revisa $LOG para el detalle."
