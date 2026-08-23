#!/bin/bash
set -euo pipefail

root="${1:?local sysroot required}"
shift
for package in "$@"; do
    dpkg-deb -x "$package" "$root"
done
