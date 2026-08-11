#!/usr/bin/env bash
# 3DS build wrapper: run via an MSYS2 login shell, which starts in $HOME --
# this script exists so the cd can never be dropped from an inline -lc string
# (it has now been dropped by three different models; see Claude memory
# ctr-3ds-build-via-script-not-inline).
cd /c/Users/toddc/Documents/CTR-native/ctr-native-work || exit 1
cmake --build build-3ds 2>&1
exit ${PIPESTATUS[0]}
