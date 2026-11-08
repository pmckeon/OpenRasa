// Audio.h - TRasa::AudioManager (audiomanager.cpp; the original plays through the Miles Sound System, mss32.dll), on SDL3_mixer.
//
// The scripts register the audio database at start-up (client/audiomgr.py from generated.client.audiosetdata): sounds (id, file,
// description, volume), categories (description, priority, max simultaneous, EAX, 3D, music, fade in / out ms, fade curve), audio
// sets (description, category, looping, selection type 1 random / 2 sequential / 3 simultaneous, min / max distance, silence between
// tracks in seconds, mufflable) and set members. Everything plays as an audio set: CreateAudioRequest(set) -> request.Play() - the
// scripts (backgroundaudiomgr.py PlayAudioSet, StartMusic) and the UI's widget sounds alike (UIManager + 0x9C = FUN_004d8000:
// CreateAudioRequest(id) and Play). Volume of a playing sample (FUN_007b0d50): the sound's volume x the request's x the music or
// effect volume (Client.Audio.MusicVolume / EffectVolume / 100), the master volume (Client.Audio.MasterVolume) on the whole mix.
#pragma once

#include <Python.h>

#include "sky.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::audio {

// The client.cfg settings (Client.Audio.Enabled, MasterVolume, MusicVolume, EffectVolume - volumes 0..1 -, MaxHandles - the voices,
// a byte -, UpdatePeriodMilliseconds - how often audibility is refreshed -, PlaybackFrequency).
void configure(bool enabled, float master, float music, float effects, unsigned maxHandles, unsigned updatePeriodMs,
               unsigned playbackFrequency,    // Client.Audio.PlaybackFrequency: Miles' driver rate (the occlusion filter's reference)
               unsigned playbackBits,         // Client.Audio.PlaybackBits: the driver's sample size (taken when the device opens)
               float detailLevel);            // Client.Audio.DetailLevel / 100: AudioSet detail (FUN_007a07c0; no sets yet at start-up)
PyObject* managerObject();                 // gameclient.GetAudioManager() (new reference)
bool playAudioSet(int audioSetId);         // a UI sound (FUN_004d8000)
// The audio manager's frame: requests start, wait or stop by audibility within the voices (Audio.cpp update()).
void update(int msElapsed);
// 3D sounds (TRasa::Audio3D, audio3d.cpp; Miles AIL 3D samples). The listener is the camera (the camera update FUN_0056da80 ->
// FUN_007a28b0: AIL_set_listener_3D_position / _orientation with the camera's position and orientation, once a frame). A 3D sound's
// volume x the falloff the exe registers for every 3D sample (AIL_register_falloff_function_callback, LAB_007b37e0):
// ((max - d) / (max - min))^2 between the min and max distance (AIL_set_sample_3D_distances(max, min)); full within min, silent beyond
// max; left / right as Miles' stereo speaker levels (Audio.cpp stereoLevels, traced in mss32.dll).
void setListener(const std::array<float, 3>& position, const std::array<float, 3>& forward, const std::array<float, 3>& up);
// body.AttachAudio(request) (0x004FE720 -> FUN_00502eb0): the request joins the body's sounds (+ 0xc8), takes the body's position
// (its node's world position, slot 0x14) and plays (slot 0x20, the category's fade) - it follows the body while it plays.
PyObject* attachToEntity(PyObject* request, uint64_t entity);
PyObject* detachFromEntity(PyObject* request, uint64_t entity);   // body.DetachAudio
void stopEntitySounds(uint64_t entity);
// Special-FX package sound events (TRasa::SfxAudioEvent trigger FUN_00760210, product SfxAudioProduct FUN_00775200): CreateAudioRequest
// (set) placed at the event's point and played; a body below the SFX relevance threshold plays only categories whose name starts
// "Music" (category + 0x24). Returns the product's handle, 0 when nothing plays. The product moves the request with its connection
// point each update (FUN_007753f0) and lets go of it when it is done (state 3); stopping the package (FUN_00775580) or removing the body
// fades it out over the event's fade time when the event says so (+ 0x2c / + 0x30) and lets go of it - a one-shot keeps playing.
uint64_t playPackageSound(int audioSet, const std::array<float, 3>& position, bool lowRelevance);
bool movePackageSound(uint64_t handle, const std::array<float, 3>& position);   // false once the request is done
void releasePackageSound(uint64_t handle, bool fade, int fadeMs);                           // body.Destroy: attached sounds stop at once
// The map's audio emitters (gameclient.LoadAudioEmitters(map) -> the map folder's audioemitters.xml; ClearAudioEmitters).
void loadEmitters(const std::vector<tr::AudioEmitter>& emitters);
void clearEmitters();
// The regions' ambient sound (RegionManagerClient TransitionAmbientSound): the set the first region at the camera with enabledFlags bit 2
// names, 0 for none; a looping request swapped when the set changes (5000 ms fades; immediate stops the old one at once).
void setRegionAmbient(int audioSet, bool immediate);
// The regions' music (RegionManagerClient TransitionMusicTrack -> MusicManager::SetMusicTracks): flag = some region at the camera has
// enabledFlags bit 3, audioSet its music set; played, then replayed after 60 - 93 s of silence; changed with a 5000 ms fade.
void setRegionMusic(bool flag, int audioSet, bool immediate);
float masterGain();                        // Client.Audio.MasterVolume, 0 with audio off (movies: MoviePlayer.h)                // once a frame: tracks that ended, the silences between looping tracks
// Whether the audio manager is running (audio on and the device open; GameClient + 0x238 with + 0x108 set) - voice chat needs it.
bool running();
void shutdown();

}  // namespace engine::audio
