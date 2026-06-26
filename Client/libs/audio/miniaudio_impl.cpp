// The single translation unit that compiles the miniaudio implementation. Keeping MA_IMPLEMENTATION
// isolated here (a) avoids ODR violations and (b) keeps AudioEngine.cpp recompiling fast (the impl is
// ~90k lines). We don't need capture/encoding/decoding-to-file, so trim a few unused subsystems.

#define MA_IMPLEMENTATION
#define MA_NO_ENCODING       // playback only — no writing audio files
#define MA_NO_GENERATION     // no waveform/noise generators

#include <miniaudio.h>
