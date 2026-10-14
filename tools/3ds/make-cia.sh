#!/bin/sh
# Empaqueta un .elf de 3DS como .cia con ícono y banner.
# Requiere makerom y bannertool en el PATH.
#
# Uso:
#   make-cia.sh <elf> <salida.cia> <titulo> <descripcion> <autor> \
#               <unique_id> <product_code> <icon.png> <banner.png> <banner.wav>
#
# Las opciones de New 3DS (804 MHz, caché L2, 124 MB) se activan siempre:
# este proyecto es para New 3DS y en una Old 3DS se ignoran.
set -eu

if [ "$#" -ne 10 ]; then
    sed -n '2,8p' "$0"
    exit 1
fi

ELF=$1 OUT=$2 TITLE=$3 DESC=$4 AUTHOR=$5
UNIQUE_ID=$6 PRODUCT_CODE=$7 ICON=$8 BANNER=$9
shift 9
AUDIO=$1

DIR=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

bannertool makesmdh -s "$TITLE" -l "$DESC" -p "$AUTHOR" -i "$ICON" -o "$WORK/icon.icn"
bannertool makebanner -i "$BANNER" -a "$AUDIO" -o "$WORK/banner.bnr"

makerom -f cia -o "$OUT" -elf "$ELF" -rsf "$DIR/app.rsf" -target t \
    -exefslogo -logo "$DIR/logo.bcma.lz" \
    -icon "$WORK/icon.icn" -banner "$WORK/banner.bnr" \
    -DAPP_TITLE="$TITLE" -DAPP_PRODUCT_CODE="$PRODUCT_CODE" \
    -DAPP_UNIQUE_ID="$UNIQUE_ID" -DAPP_CATEGORY=Application \
    -DAPP_USE_ON_SD=true -DAPP_ENCRYPTED=false -DAPP_MEMORY_TYPE=Application \
    -DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=124MB \
    -DAPP_CPU_SPEED=804MHz -DAPP_ENABLE_L2_CACHE=true -DAPP_VERSION_MAJOR=0

echo "CIA generado: $OUT"
