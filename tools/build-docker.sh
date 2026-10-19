#!/usr/bin/env bash
# Linux/macOS: runs tools/build.sh inside the Docker build image (see Dockerfile), so
# no Android SDK, NDK or compilers are needed on the host.
#
#   tools/build-docker.sh picker  --game ~/Downloads/CLAW-1.4.5.4-Complete-Pack.zip
#   tools/build-docker.sh bundled --game /path/to/CLAW-1.4.5.4
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=claw-android-build
command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }

MODE=${1:?usage: tools/build-docker.sh picker|bundled --game <pack zip | game folder> [options]}
shift
ARGS=() GAME=""
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME="$(realpath "${2:?}")"; shift 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done
[ -n "$GAME" ] || { echo "--game <pack zip | game folder> is required" >&2; exit 1; }

docker image inspect $IMAGE >/dev/null 2>&1 || docker build -t $IMAGE "$ROOT"
if [ -f "$GAME" ]; then MNT="$GAME:/game/pack.zip:ro" ARG=/game/pack.zip; else MNT="$GAME:/game/folder:ro" ARG=/game/folder; fi
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" -v "$MNT" \
    -e HOME=/src/build/home -e GRADLE_USER_HOME=/src/build/gradle-home \
    $IMAGE bash tools/build.sh "$MODE" --game "$ARG" "${ARGS[@]}"
