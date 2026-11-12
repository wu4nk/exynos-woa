#!/usr/bin/env bash
# mkclips-s10.sh <out dir> - the test media of tools/s10-kit/video-s10.ps1 (kit video\), made on Linux with ffmpeg
# (libx264) + a tiny C hasher, no PyAV:
#   mfc_test.h264        the Exynos9820Mfc boot self-test vector (mkh264pcm.py; its 3 known frame hashes in
#                        mfc_test.ref.txt)
#   c720.mp4 / c1080.mp4 90-frame test patterns: 720p Main 2 B-frames, 1080p High 3 B-frames 4 refs (the 1080 one is
#                        coded 1920x1088, cropped)
#   c1080.h264           c1080.mp4's video as Annex-B (mfcdec reads raw streams)
#   *.ref.txt            libavcodec's decode of each clip: per displayed frame, FNV-1a 32 of the cropped luma plane
#                        and of the NV12 CbCr plane - the line format mfcdec / mftest print (same hash, row order)
#   play.mp4             20 s 1080p30 High + AAC: the playback check (Media Player / Edge, MFC busy probe)
set -euo pipefail
O=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd); H=$(cd "$(dirname "$0")" && pwd)
q=(-hide_banner -loglevel error -y)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/fnv.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v)
{
    size_t w = strtoul(v[2], 0, 10), h = strtoul(v[3], 0, 10), n = w * h * 3 / 2, k;
    unsigned char *b = malloc(n);
    FILE *f = fopen(v[1], "rb");
    int i = 0;
    while (f && fread(b, 1, n, f) == n) {
        unsigned y = 0x811C9DC5u, uv = 0x811C9DC5u;
        for (k = 0; k < w * h; k++) y = (y ^ b[k]) * 0x01000193u;
        for (; k < n; k++) uv = (uv ^ b[k]) * 0x01000193u;
        printf("frame %d: luma fnv 0x%08x chroma fnv 0x%08x\n", i++, y, uv);
    }
    return c < 4;
}
EOF
cc -O2 -o "$T/fnv" "$T/fnv.c"
python3 -I "$H/mkh264pcm.py" "$O/mfc_test.h264" | grep '^frame' > "$O/mfc_test.ref.txt"
ffmpeg "${q[@]}" -f lavfi -i testsrc2=s=1280x720:r=30 -frames:v 90 -c:v libx264 -profile:v main -bf 2 \
       -pix_fmt yuv420p -movflags +faststart "$O/c720.mp4"
ffmpeg "${q[@]}" -f lavfi -i testsrc2=s=1920x1080:r=30 -frames:v 90 -c:v libx264 -profile:v high -bf 3 -refs 4 \
       -pix_fmt yuv420p -movflags +faststart "$O/c1080.mp4"
ffmpeg "${q[@]}" -i "$O/c1080.mp4" -c:v copy -bsf:v h264_mp4toannexb -f h264 "$O/c1080.h264"
for c in c720:1280:720 c1080:1920:1080; do
  IFS=: read -r n w h <<< "$c"
  ffmpeg "${q[@]}" -i "$O/$n.mp4" -f rawvideo -pix_fmt nv12 "$T/$n.nv12"
  "$T/fnv" "$T/$n.nv12" "$w" "$h" > "$O/$n.ref.txt"
  rm -f "$T/$n.nv12"
done
ffmpeg "${q[@]}" -f lavfi -i testsrc2=s=1920x1080:r=30 -f lavfi -i sine=f=440:sample_rate=48000 -t 20 \
       -c:v libx264 -profile:v high -b:v 4M -maxrate 5M -bufsize 8M -pix_fmt yuv420p -c:a aac -b:a 96k \
       -movflags +faststart "$O/play.mp4"
cp "$H/mkh264pcm.py" "$O/"
(cd "$O" && wc -l ./*.ref.txt && sha256sum ./*.mp4 ./*.h264 > SHA256SUMS && cat SHA256SUMS)
