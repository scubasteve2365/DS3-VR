#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ds3vr {

// The mod menu (Dear ImGui, the same library UEVR, Mutar and Luke Ross use). Opened and closed by
// clicking both thumbsticks together on the gamepad, or the Insert key. It is drawn into a texture
// of its own, which the VR side shows as a panel in front of you; while it is open the game does not
// see the gamepad. Every change is saved to ds3vr.ini straight away.

void MenuInstallInputHooks();                              // once, from the init thread (XInput)
void MenuTick(ID3D11Device* dev, ID3D11DeviceContext* c);  // every Present, before the VR frame
bool MenuIsOpen();
unsigned MenuOpenGeneration();                             // changes each time the menu opens
ID3D11Texture2D* MenuTexture();                            // null until first drawn
unsigned MenuContentHeight();                              // rows of MenuTexture() the menu uses (top part)

} // namespace ds3vr
