#pragma once
// Copy this file to src/secrets.h (which is gitignored) and fill it in. Nothing here is
// read unless that copy exists; see "Live Cam" in config.h.

// Any plain-HTTP URL that answers with either an MJPEG stream (multipart/x-mixed-replace,
// e.g. go2rtc's /api/stream.mjpeg?src=NAME) or a single JPEG (a snapshot URL, which is
// then fetched over and over). Frames must be baseline JPEG. https:// is not supported.
#define LIVECAM_URL  "http://192.168.1.10:1984/api/stream.mjpeg?src=camera"

// Keep frames small: about 466 wide and under ~5 KB each. Larger ones stalled on a real Orb.
// With go2rtc, e.g.: ffmpeg ... -vf scale=466:262,fps=3 -q:v 28 -f mpjpeg pipe:1

// Optional: what the knob menu calls the app.
// #define LIVECAM_NAME "Garden"
