#!/bin/sh
# Linux/macOS convenience launcher, equivalent of run.bat.
cd "$(dirname "$0")"
exec ./build/bin/game "$@"
