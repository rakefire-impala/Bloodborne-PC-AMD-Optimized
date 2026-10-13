"""Run the AudioOut port pacing against a fake SDL device that stops taking audio for 3 s
(a Bluetooth headset dropping out) and then plays on: the queue must be back at its target
level afterwards, not seconds behind."""
from pathlib import Path
import argparse
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='cc')
parser.add_argument('--baseline', default='edc668059de3dd2fea3a3c35d478a5fa423a7fac')
args = parser.parse_args()

FAKE_SDL = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { SDL_AUDIO_S16 = 0x8010, SDL_AUDIO_F32 = 0x8120 } SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef uint32_t SDL_AudioDeviceID;
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
#define SDL_INIT_AUDIO 0x10u
typedef struct SDL_AudioStream { int queued; } SDL_AudioStream;
bool SDL_InitSubSystem(uint32_t flags);
const char *SDL_GetCurrentAudioDriver(void);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
                                           void *callback, void *userdata);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int bytes);
bool SDL_ClearAudioStream(SDL_AudioStream *stream);
'''

FIXTURE = r'''
#include "runtime_audio.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; return 0;
}
void compat_sleep_ns(uint64_t ns) { (void)ns; }

/* One device: the stream bound to it, whether it plays, and what the test saw. */
static SDL_AudioStream *bound;
static int opens, clears;
bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
const char *SDL_GetCurrentAudioDriver(void) { return "fake"; }
const char *SDL_GetError(void) { return ""; }
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
                                           void *callback, void *userdata) {
    (void)device; (void)spec; (void)callback; (void)userdata;
    static SDL_AudioStream streams[8];
    assert(opens < 8);
    bound = &streams[opens++];
    bound->queued = 0;
    return bound;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) { (void)stream; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream *stream) { if (stream == bound) bound = NULL; }
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) { return stream->queued; }
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int bytes) {
    (void)data; stream->queued += bytes; return true;
}
bool SDL_ClearAudioStream(SDL_AudioStream *stream) { stream->queued = 0; ++clears; return true; }

int main(void) {
    assert(audio_init() == 0);
    /* FMOD's main port: 256 frames, 8 channels, float. */
    const int32_t handle = audio_open(0, 0, 0, 256, 48000, 5);
    assert(handle > 0);
    const int bytes = 256 * 8 * 4;
    static float data[256 * 8];
    int stalled_peak = 0, clears_playing = 0;
    /* 2 s playing, 3 s with the device gone, 3 s playing again; 187.5 buffers a second. The
       device takes two buffers every second call: a 10.7 ms quantum. */
    for (int i = 0; i < 1500; ++i) {
        const int playing = i < 375 || i >= 938;
        if (playing && bound && i % 2 == 0) {
            bound->queued -= bound->queued < 2 * bytes ? bound->queued : 2 * bytes;
        }
        const int before = clears;
        assert(output_port(handle, data, 0) == 256 * 8);
        if (i < 375) clears_playing += clears - before;
        if (!playing && bound && bound->queued > stalled_peak) stalled_peak = bound->queued;
    }
    const int left = bound ? bound->queued : -1;
    printf("queued after the outage %.1f ms (target %.1f ms), at most %.0f ms while it lasted; "
           "%d drops while playing, %d streams opened\n",
           left * 1000.0 / (48000.0 * 8 * 4), 2 * bytes * 1000.0 / (48000.0 * 8 * 4),
           stalled_peak * 1000.0 / (48000.0 * 8 * 4), clears_playing, opens);
    /* Back at the target (two buffers, plus the one put and a device quantum); nothing dropped
       while the device played normally. */
    return left >= 0 && left <= 5 * bytes && clears_playing == 0 ? 0 : 1;
}
'''

with tempfile.TemporaryDirectory(prefix='bb-audio-backlog-') as directory:
    temp = Path(directory)
    (temp/'SDL3').mkdir()
    (temp/'SDL3/SDL.h').write_text(FAKE_SDL)
    for label, old, expected in [('Before', True, 1), ('production behavior', False, 0)]:
        path = 'src/runtime_audio.c'
        source = (subprocess.check_output(['git', 'show', args.baseline+':'+path], cwd=ROOT).decode()
                  if old else (ROOT/path).read_text(encoding='utf-8'))
        (temp/'runtime_audio.c').write_text(source, encoding='utf-8')
        (temp/'fixture.c').write_text(FIXTURE)
        subprocess.run(shlex.split(args.compiler)+['-std=gnu11', '-O1', '-pthread', '-Wall', '-Wno-unused-function',
                       '-I'+str(temp), '-I'+str(ROOT/'src'), str(temp/'fixture.c'), '-o', str(temp/'fixture')],
                       check=True)
        result = subprocess.run([str(temp/'fixture')], capture_output=True, text=True, timeout=60)
        print(label+': '+result.stdout.strip().splitlines()[-1], flush=True)
        assert result.returncode == expected, (label, result.stdout, result.stderr)
print('PASS: audio queued while the output device is gone is dropped once it plays again.')
