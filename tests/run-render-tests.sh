#!/usr/bin/env bash
# Builds the plugin without its UI, builds the render tests against the libobs
# on this system, makes the test clips, and runs the tests under Xvfb. See
# tests/README.md for what it needs installed.
#
#   tests/run-render-tests.sh [--verbose]
#
# The pictures the tests checked are left in build_tests/render-out as PNGs.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${root}/build_tests"
out="${build}/render-out"
media="${build}/media"

multiarch="$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"
obs_plugins="${OBS_PLUGINS_DIR:-/usr/lib/${multiarch}/obs-plugins}"
obs_data="${OBS_DATA_DIR:-/usr/share/obs/obs-plugins}"

for tool in cmake ffmpeg xvfb-run; do
  if ! command -v "${tool}" >/dev/null; then
    echo "${tool} is needed to run the render tests" >&2
    exit 2
  fi
done

# the plugin, with nothing that needs the OBS window
cmake -S "${root}" -B "${build}/plugin" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_FRONTEND_API=OFF -DENABLE_QT=OFF -Wno-dev --log-level=ERROR
cmake --build "${build}/plugin"

cmake -S "${root}/tests" -B "${build}/harness" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo --log-level=ERROR
cmake --build "${build}/harness"

plugin="$(find "${build}/plugin" -maxdepth 2 -name 'Warp.so' | head -n 1)"

# two different-looking clips of four seconds, with sound
mkdir -p "${media}" "${out}"
make_clip() {
  local name="$1" pattern="$2"
  if [[ ! -f "${media}/${name}" ]]; then
    ffmpeg -loglevel error -y -f lavfi -i "${pattern}=size=640x360:rate=30:duration=4" \
      -f lavfi -i "sine=frequency=440:duration=4" -c:v libx264 -pix_fmt yuv420p -g 30 \
      -c:a aac -shortest "${media}/${name}"
  fi
}
make_clip clip-4s.mp4 testsrc2
make_clip clip-4s-b.mp4 smptebars
# an image to stand in for a glyph
if [[ ! -f "${media}/red-square.png" ]]; then
  ffmpeg -loglevel error -y -f lavfi -i "color=c=red:size=64x64" -frames:v 1 "${media}/red-square.png"
fi

rm -f "${out}"/*.pam "${out}"/*.png

status=0
xvfb-run -a -s "-screen 0 1280x720x24" "${build}/harness/warp-render-tests" \
  --plugin "${plugin}" --data "${root}/data" \
  --obs-plugins "${obs_plugins}" --obs-data "${obs_data}" \
  --media "${media}" --out "${out}" "$@" || status=$?

# the pictures as PNGs, to look at
for pam in "${out}"/*.pam; do
  [[ -e "${pam}" ]] || continue
  ffmpeg -loglevel error -y -i "${pam}" "${pam%.pam}.png" && rm -f "${pam}"
done

echo "pictures: ${out}"
exit "${status}"
