#!/usr/bin/env bash
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$repo_root/scripts/build-linux.sh" --qt-root /home/jules/Qt/6.9.2/gcc_64 --configuration Release --use-quickjs
