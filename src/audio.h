// XAudio2 runs the game's voice callbacks on its own thread, which would change game memory while a savestate is
// taken or loaded. The engine is stopped around both (it is created through CoCreateInstance, which is hooked).
#pragma once

namespace audio {

void InstallHooks();
void Stop();
void Start();
void Report(const char* when);   // logs how many voices play and the glitch count (diagnostics)

}  // namespace audio
