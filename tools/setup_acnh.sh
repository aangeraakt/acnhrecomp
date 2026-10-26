#!/usr/bin/env bash
# The setup is tools/setup_acnh.py, the same on Linux, macOS and Windows (README, "Installation"): this runs it with
# the same arguments, for the scripts and habits that call it by this name.
cd "$(dirname "$0")/.." || exit 1
exec python3 tools/setup_acnh.py "$@"
