#pragma once
// OPTIONAL. The Livestream app does not need this file: set its camera URL on the Orb in
// Settings > Livestream, or from a browser at http://theorb.local/livestream.
//
// For development, copy this file to src/secrets.h (which is gitignored) to give a fresh
// build a URL to use until one has been set on the device.

// Any plain-HTTP URL that answers with either an MJPEG stream (multipart/x-mixed-replace,
// e.g. go2rtc's /api/stream.mjpeg?src=NAME) or a single JPEG (a snapshot URL, which is
// then fetched over and over). Frames must be baseline JPEG. https:// is not supported.
#define LIVECAM_URL  "http://192.168.1.10:1984/api/stream.mjpeg?src=camera"

// Keep frames small: about 466 wide and under ~5 KB each. Larger ones stalled on a real Orb.
// With go2rtc, e.g.: ffmpeg ... -vf scale=466:262,fps=3 -q:v 28 -f mpjpeg pipe:1
