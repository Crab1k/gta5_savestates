// A short message in the top-left corner of the game's picture ("Savestate 1 saved ..."), drawn into the back buffer
// just before the game presents it. The text is rendered with GDI into a texture whenever it changes.
#pragma once
#include <string>

struct IDXGISwapChain;
struct ID3D11Device;

namespace overlay {

void Install(IDXGISwapChain* swapChain, ID3D11Device* device);
void Show(const std::string& text, unsigned ms);
void WaitFrames(int frames, unsigned timeoutMs);   // until the game has presented this many more frames

}  // namespace overlay
