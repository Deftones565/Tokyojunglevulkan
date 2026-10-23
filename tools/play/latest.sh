#!/bin/bash
# usage: tools/play/latest.sh <run-name> [out.png]  -- newest dumped frame as PNG (half size)
d="$(cd "$(dirname "$0")/../.." && pwd)/build-runs/$1"
f=$(ls $d/frames | tail -1); out=${2:-$d/latest.png}
python3 -c "from PIL import Image; Image.open('$d/frames/$f').resize((640,360)).save('$out')"
echo "$f -> $out"; tail -1 $d/log.txt | cut -c1-160
