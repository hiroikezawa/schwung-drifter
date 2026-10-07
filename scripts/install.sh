#!/usr/bin/env bash
# Install the built tarball (dist/drifter-module.tar.gz) through Schwung Manager.
#   scripts/install.sh [http://<move-ip>:7700]
set -euo pipefail
exec python3 "$(dirname "$0")/install.py" "$@"
