#!/usr/bin/env bash
# Fetch libsqlite3 dev headers + runtime lib WITHOUT sudo, into /tmp/sqlite_local.
# Uses apt-get download (no root) + dpkg-deb -x (extract only, no install).
set -e
DEST=/tmp/sqlite_local
rm -rf "$DEST"
mkdir -p "$DEST"
cd "$DEST"
apt-get download libsqlite3-dev libsqlite3-0 >/dev/null 2>&1
for d in *.deb; do dpkg-deb -x "$d" .; done
echo "header: $(find "$DEST" -name sqlite3.h | head -1)"
echo "libs:"
find "$DEST" -name 'libsqlite3*' -printf '  %p\n'
