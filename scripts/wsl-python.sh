#!/bin/bash
# pyfa's Python on WSL/Linux (.venv from scripts/setup-wsl-venv.sh), with the extracted
# shared libraries wxPython needs. Usage: scripts/wsl-python.sh pyfa.py --savepath DIR ...
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$ROOT/.venv/syslib/usr/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH="$LIB:$LIB/pulseaudio${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ROOT/.venv/bin/python" "$@"
