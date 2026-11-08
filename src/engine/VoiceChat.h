// VoiceChat.h - squad voice chat: the exe's Talkback client (TRasa::VoiceChatClient over Talkback::ChatClient / ChatClientImpl,
// ChatClientImpl.cpp, AudioInThread / AudioOutThread / AudioOutStream.cpp, SpeexCodec.cpp; netUdpOutConnection.cpp underneath).
//
// The scripts drive it (client/party.py): Recv_VoiceChatConnectInfo -> InitVoiceChat, ConnectToVoiceServer("host:port");
// OnVoiceServerConnected -> LoginToVoiceGroup(group, player, token); push-to-talk (inputhandlers.py PushToTalk) -> BeginSpeaking /
// EndSpeaking; leaving -> DisconnectFromVoiceServer, ShutdownVoiceChat. The client calls back into party.py (the client party
// manager, script entity 0xb): OnVoiceServerConnected, OnVoiceServerLogin, OnVoiceServerLoginFailed, OnVoiceServerDisconnected,
// OnBeginSpeaking, OnEndSpeaking, OnPlayerAddedToVoiceGroup, OnPlayerRemovedFromVoiceGroup. The wire format matches Rasa.NET's
// Rasa.Game/Voice/TalkbackProtocol.cs; the details below are from the exe.
#pragma once

#include <Python.h>

#include <cstdint>
#include <string>

namespace engine::voice {

// How the client reaches the scripts: entitymanager.AddMethodCall on entity 0xb (FUN_00536900; delay 0 = the next run, -1 = at once).
using ScriptCaller = void (*)(const char* method, PyObject* args, int delayMs);
void setScriptCaller(ScriptCaller caller);

// gameclient.InitVoiceChat (FUN_004d52c0): makes the client when there is none - needs the audio manager running. voiceActivation =
// Client.Voice.Activated and not ListenOnly; activationLevel / cutoffDelay / volume as clamped there. False when it cannot.
bool init(bool voiceActivation, int activationLevel, int cutoffDelay, int volume);
bool exists();
bool connected();                                    // ChatClientImpl + 0x5c (FUN_00a90350)
bool loggedIn();                                     // + 0x5e (FUN_00a90320)
bool talking();                                      // + 0x188 (FUN_00a902f0)
bool connect(const std::string& hostAndPort);        // ConnectToVoiceServer (FUN_004d56b0 -> FUN_00a8dea0)
bool login(uint32_t group, uint64_t player, uint32_t token);   // LoginToVoiceGroup (FUN_004d5770 -> FUN_00a8dca0)
void disconnect();                                   // Disconnect (FUN_00a8d790)
void shutdown();                                     // ShutdownVoiceChat (FUN_004d58c0): disconnects, deletes the client
void startTalking();                                 // StartTalking (FUN_00a8db90)
void stopTalking();                                  // StopTalking (FUN_00a8ca50)
void setVolume(int percent);                         // FUN_004d5af0: Client.Voice.VoiceVolume 0 - 100
// The client's frame (ChatClientImpl slot 2, FUN_00a8fe10): the connection, the server's messages, the heartbeat, the microphone.
void update();

}  // namespace engine::voice
