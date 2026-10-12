#!/bin/bash
# ==============================================================================
# make_rip.sh - GTA III (re3) Asset Ripper / Optimizer for SF2000 / GB300
#
# Reduces the ~1 GB desktop GTA III installation down to ~350 MB by:
#  - Replacing 506 MB of large radio station .wav files with 44-byte dummy WAVs
#  - Omitting intro/logo movies (38 MB)
#  - Omitting desktop binaries, PDFs, and temporary logs
#  - Preserving all SFX (sfx.raw/sdt), cutscene audio, missions, and textures
# ==============================================================================

set -euo pipefail

SRC_DIR="${1:-"/home/Sajnaps/gta3-data/gta3_(re3,_v_3233ffe_02.09.2021)_[Linux,_amd64,_1C-Assets](3)/game"}"
DST_DIR="${2:-"/home/Sajnaps/gta3-rip"}"

if [ ! -d "$SRC_DIR" ]; then
    echo "Error: Source directory '$SRC_DIR' does not exist."
    exit 1
fi

echo "========================================================"
echo " GTA III RIP Builder for SF2000 / GB300"
echo " Source: $SRC_DIR"
echo " Destination: $DST_DIR"
echo "========================================================"

mkdir -p "$DST_DIR"

# 1. Essential game data folders
for folder in anim data models skins text txd; do
    if [ -d "$SRC_DIR/$folder" ]; then
        echo "Copying $folder/..."
        mkdir -p "$DST_DIR/$folder"
        cp -a "$SRC_DIR/$folder/." "$DST_DIR/$folder/"
    fi
done

# 2. Audio processing
echo "Processing audio/..."
mkdir -p "$DST_DIR/audio"

# Radio stations to replace with dummy WAVs (506 MB saved)
RADIO_STATIONS=(
    "chat.wav"
    "city.wav"
    "class.wav"
    "flash.wav"
    "game.wav"
    "head.wav"
    "kjah.wav"
    "lips.wav"
    "msx.wav"
    "police.wav"
    "rise.wav"
    "water.wav"
)

# Copy all non-radio audio files (sfx.raw, sfx.sdt, speech mp3s/wavs)
for f in "$SRC_DIR/audio"/*; do
    fname=$(basename "$f")
    is_radio=0
    for r in "${RADIO_STATIONS[@]}"; do
        if [ "$fname" = "$r" ]; then
            is_radio=1
            break
        fi
    done

    if [ "$is_radio" -eq 0 ]; then
        cp -a "$f" "$DST_DIR/audio/"
    fi
done

# Create 44-byte standard PCM WAV header for dummy stations
python3 -c "
import struct, sys

def create_dummy_wav(path):
    channels = 2
    sample_rate = 22050
    bits = 16
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size = 0
    header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_size, b'WAVE',
        b'fmt ', 16, 1, channels, sample_rate, byte_rate, block_align, bits,
        b'data', data_size
    )
    with open(path, 'wb') as f:
        f.write(header)

for path in sys.argv[1:]:
    create_dummy_wav(path)
" $(for r in "${RADIO_STATIONS[@]}"; do echo "$DST_DIR/audio/$r"; done)

echo "Created dummy headers for ${#RADIO_STATIONS[@]} radio stations."

# 3. Create trigger file & copy config if present
touch "$DST_DIR/re3.sf2k"
if [ -f "$SRC_DIR/re3.ini" ]; then
    cp -a "$SRC_DIR/re3.ini" "$DST_DIR/re3.ini"
fi
mkdir -p "$DST_DIR/mp3"

# 4. Summary
SRC_SIZE=$(du -sh "$SRC_DIR" | cut -f1)
DST_SIZE=$(du -sh "$DST_DIR" | cut -f1)

echo "========================================================"
echo " RIP Complete!"
echo " Original size : $SRC_SIZE"
echo " Trimmed size  : $DST_SIZE"
echo " Target path   : $DST_DIR"
echo "========================================================"
