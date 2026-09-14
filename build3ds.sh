#!/usr/bin/env bash
# Run via an MSYS2 login shell (`bash.exe -l build3ds.sh`). The cd comes
# first because a login shell starts in $HOME, so a bare `cmake --build`
# would run against the wrong directory.
cd "$(dirname "$(readlink -f "$0")")" || exit 2
cmake --build build-3ds 2>&1
exit ${PIPESTATUS[0]}
