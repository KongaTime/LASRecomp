#!/usr/bin/env bash
# LAS Recomp on the PS5 - copy the built title folder to the console over FTP.
#
#   PS5_HOST=<console address> ps5/tools/deploy.sh
#
# Uploads build/ps5/dist/<TITLE_ID>/ to /data/homebrew/<TITLE_ID>/ through the console's FTP server (an FTP payload
# such as etaHEN's, port 2121; FTP_PORT to change it). Only the title's own files: the game files (extracted/) are
# copied once, separately (ps5/README.md); what is already there - extracted/, save/, larecomp.ini - stays.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
host=${PS5_HOST:?PS5_HOST=<the console address>}
port=${FTP_PORT:-2121}
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
app="$root/build/ps5/dist/$title_id"
[[ -f $app/eboot.bin ]] || { echo "no title at $app: ps5/tools/build.sh first" >&2; exit 2; }
cd "$app"
find . -type f ! -path './extracted/*' ! -path './save/*' -print0 | while IFS= read -r -d '' f; do
    curl -sS --ftp-create-dirs -T "$f" "ftp://$host:$port/data/homebrew/$title_id/${f#./}"
done
echo "deployed $title_id to the console"
