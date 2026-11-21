#pragma once
// Colours shared by Ui.cpp and UiPro.cpp.

#include <imgui.h>

namespace col
{
static const ImVec4 bg(0.067f, 0.075f, 0.090f, 1.0f);        // #111317
static const ImVec4 panel(0.102f, 0.114f, 0.137f, 1.0f);     // #1A1D23
static const ImVec4 panel2(0.133f, 0.149f, 0.180f, 1.0f);    // #22262E
static const ImVec4 border(0.165f, 0.184f, 0.220f, 1.0f);    // #2A2F38
static const ImVec4 text(0.925f, 0.933f, 0.949f, 1.0f);      // #ECEEF2
static const ImVec4 muted(0.588f, 0.612f, 0.659f, 1.0f);     // #969CA8
static const ImVec4 accent(0.369f, 0.627f, 1.0f, 1.0f);      // #5EA0FF
static const ImVec4 accentHi(0.475f, 0.698f, 1.0f, 1.0f);
static const ImVec4 good(0.239f, 0.839f, 0.549f, 1.0f);      // #3DD68C
static const ImVec4 warn(0.961f, 0.722f, 0.294f, 1.0f);      // #F5B84B
static const ImVec4 bad(0.941f, 0.380f, 0.427f, 1.0f);       // #F0616D
}

static inline ImU32 U32(const ImVec4& c, float a = 1.0f)
{
	return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a));
}

// UTF-8 for the few symbols the UI uses (the fonts load these ranges).
#define U8_DEG "\xC2\xB0"
#define U8_PLUSMINUS "\xC2\xB1"
#define U8_DOT "\xC2\xB7"
#define U8_INF "\xE2\x88\x9E"
