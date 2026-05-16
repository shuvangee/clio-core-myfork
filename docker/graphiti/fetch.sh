#!/bin/bash
# Clone graphiti into external/graphiti.
#
# graphiti is no longer a git submodule of iowarp-core. The
# docker/graphiti/docker-compose.yml graphiti service builds from
# `context: ../../external/graphiti`, so that directory must exist
# before `docker compose ... build`. Run this script first (it is
# idempotent — skips the clone if the checkout is already present).
#
# Usage:
#   docker/graphiti/fetch.sh
#   docker compose -f docker/graphiti/docker-compose.yml up
set -euo pipefail

GRAPHITI_URL="${GRAPHITI_URL:-https://github.com/getzep/graphiti.git}"
DEST="$(cd "$(dirname "$0")/../../external" && pwd)/graphiti"

if [ -d "$DEST/.git" ]; then
    echo "graphiti already present at $DEST (skipping clone)"
    exit 0
fi

echo "Cloning graphiti into $DEST ..."
git clone --depth 1 "$GRAPHITI_URL" "$DEST"
echo "Done. Now run: docker compose -f docker/graphiti/docker-compose.yml up"
