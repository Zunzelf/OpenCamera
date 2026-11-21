// Sony-style "pro" camera controls for the phone, shown under the preview:
// a row of function tiles (ISO, shutter, EV, white balance, focus, zoom,
// AE lock, light, more) that each open a small panel, plus picture aids on
// the preview itself (grid, histogram, zebra, focus peaking, level),
// rotation buttons and click-to-focus.

#include "Ui.hpp"
#include "AppState.hpp"
#include "UiTheme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
enum ProPanel { P_ISO, P_SHUTTER, P_EV, P_WB, P_FOCUS, P_ZOOM, P_AEL, P_LIGHT, P_MORE, P_COUNT };

const int kIsoStops[] = { 50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
                          2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800 };
const int kShutterStops[] = { 8000, 6400, 5000, 4000, 3200, 2500, 2000, 1600, 1250, 1000, 800, 640, 500,
                              400, 320, 250, 200, 160, 125, 100, 60, 50, 30, 25, 24, 15 };
const char* kWbNames[] = { "Auto", "Daylight", "Cloudy", "Shade", "Tungsten", "Fluorescent", "Kelvin" };
const char* kWbShort[] = { "AWB", "Daylight", "Cloudy", "Shade", "Tungsten", "Fluor.", "" };

std::string ShutterText(int64_t ns)
{
	if (ns <= 0) return "-";
	char b[24];
	if (ns >= 500000000LL) snprintf(b, sizeof(b), "%.1fs", ns / 1e9);
	else snprintf(b, sizeof(b), "1/%d", (int)std::lround(1e9 / (double)ns));
	return b;
}

std::string FocusText(float diopters)
{
	if (diopters < 0) return "-";
	if (diopters < 0.05f) return U8_INF;
	const float m = 1.0f / diopters;
	char b[24];
	if (m < 1.0f) snprintf(b, sizeof(b), "%.0f cm", m * 100.0f);
	else snprintf(b, sizeof(b), "%.1f m", m);
	return b;
}

std::string EvText(int steps, float step)
{
	const float ev = steps * step;
	char b[24];
	if (std::fabs(ev) < 0.05f) snprintf(b, sizeof(b), U8_PLUSMINUS "0.0");
	else snprintf(b, sizeof(b), "%+.1f", ev);
	return b;
}

bool SameSettings(const ProSettings& a, const ProSettings& b)
{
	return a.iso == b.iso && a.expNs == b.expNs && a.ev == b.ev && a.wb == b.wb && a.kelvin == b.kelvin &&
	       a.manualFocus == b.manualFocus && a.focus == b.focus && a.aeLock == b.aeLock && a.awbLock == b.awbLock &&
	       a.afLock == b.afLock && a.zoom == b.zoom && a.torch == b.torch && a.ois == b.ois && a.eis == b.eis &&
	       a.flicker == b.flicker;
}
} // namespace

// ------------------------------------------------------------------ small widgets
bool Ui::chip(const char* label, bool selected, bool enabled)
{
	const ImVec2 ts = ImGui::CalcTextSize(label);
	const ImVec2 size(std::max(px(52), ts.x + px(22)), px(32));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton(label, size) && enabled;
	const bool hovered = enabled && ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float a = enabled ? 1.0f : 0.35f;
	ImU32 bgc = selected ? U32(col::accent, a) : (hovered ? U32(col::border, a) : U32(col::panel2, a));
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bgc, px(8));
	dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
		selected ? IM_COL32(10, 16, 28, (int)(255 * a)) : U32(col::text, a), label);
	if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return clicked;
}

int Ui::chipGrid(const std::vector<std::string>& labels, int selected, bool enabled)
{
	int clicked = -1;
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	const float spacing = px(6);
	for (size_t i = 0; i < labels.size(); i++)
	{
		ImGui::PushID((int)i);
		if (chip(labels[i].c_str(), (int)i == selected, enabled)) clicked = (int)i;
		ImGui::PopID();
		if (i + 1 < labels.size())
		{
			const float nextW = std::max(px(52), ImGui::CalcTextSize(labels[i + 1].c_str()).x + px(22));
			if (ImGui::GetItemRectMax().x + spacing + nextW <= right) ImGui::SameLine(0, spacing);
		}
	}
	return clicked;
}

void Ui::popupTitle(const char* title, const char* hint)
{
	ImGui::PushFont(m_fontBody);
	ImGui::TextUnformatted(title);
	ImGui::PopFont();
	if (hint && *hint)
	{
		ImGui::PushFont(m_fontSmall);
		ImGui::PushTextWrapPos(0);
		ImGui::TextColored(col::muted, "%s", hint);
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	}
	ImGui::Dummy(ImVec2(0, px(2)));
}

// Small rounded button drawn over the preview. `cursor` advances to the next
// slot (to the right, or to the left when rightAligned).
bool Ui::pill(const char* id, const char* label, bool active, ImVec2& cursor, bool rightAligned)
{
	ImGui::PushFont(m_fontSmall);
	const ImVec2 ts = ImGui::CalcTextSize(label);
	const ImVec2 size(ts.x + px(22), px(28));
	const ImVec2 p = rightAligned ? ImVec2(cursor.x - size.x, cursor.y) : cursor;
	ImGui::SetCursorScreenPos(p);
	const bool clicked = ImGui::InvisibleButton(id, size);
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 bgc = active ? U32(col::accent, 0.92f) : (hovered ? IM_COL32(40, 44, 52, 225) : IM_COL32(0, 0, 0, 160));
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bgc, size.y * 0.5f);
	dl->AddText(ImVec2(p.x + px(11), p.y + (size.y - ts.y) * 0.5f), active ? IM_COL32(10, 16, 28, 255) : IM_COL32(255, 255, 255, 235), label);
	ImGui::PopFont();
	if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	cursor.x = rightAligned ? p.x - px(6) : p.x + size.x + px(6);
	return clicked;
}

// ------------------------------------------------------------------ preview tools
void Ui::drawPreviewTools(const ImVec2& pmin, const ImVec2& pmax)
{
	const int applied = g_app.appliedRotation.load();
	const int mode = g_app.rotationMode.load();

	ImVec2 left(pmin.x + px(12), pmin.y + px(12));
	char rl[40];
	snprintf(rl, sizeof(rl), "Rotate  %d" U8_DEG, applied);
	if (pill("##rot", rl, mode != 0, left, false))
	{
		// From auto, start at what is shown now, then turn a quarter clockwise.
		g_app.rotationMode = ((applied + 90) % 360) / 90 + 1;
		g_app.saveSettings();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the picture a quarter turn clockwise");
	if (pill("##autorot", "Auto", mode == 0, left, false))
	{
		g_app.rotationMode = 0;
		g_app.saveSettings();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the picture upright however the phone is held");
	if (applied == 90 || applied == 270 || g_app.fillFrame)
	{
		if (pill("##fill", "Fill 16:9", g_app.fillFrame, left, false))
		{
			g_app.fillFrame = !g_app.fillFrame;
			g_app.saveSettings();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Upright phone: crop to fill video calls instead of black side bars");
	}

	ImVec2 right(pmax.x - px(12), pmin.y + px(12));
	struct Aid { const char* id; const char* label; std::atomic<bool>* flag; const char* tip; };
	Aid aids[] = {
		{ "##level", "Level", &g_app.showLevel, "Horizon line from the phone's tilt sensor" },
		{ "##peak", "Peaking", &g_app.showPeaking, "Red outlines show what is in sharp focus" },
		{ "##zebra", "Zebra", &g_app.showZebra, "Stripes over areas that are too bright" },
		{ "##hist", "Histogram", &g_app.showHistogram, "Brightness spread: left dark, right bright" },
		{ "##grid", "Grid", &g_app.showGrid, "Rule-of-thirds lines for framing" },
	};
	for (Aid& a : aids)
	{
		if (pill(a.id, a.label, a.flag->load(), right, true))
		{
			a.flag->store(!a.flag->load());
			g_app.saveSettings();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.tip);
	}
}

void Ui::handlePreviewClick(const ImVec2& a, const ImVec2& b, bool mirror)
{
	const PhoneCaps caps = g_app.getCaps();
	if (!caps.valid || !caps.point) return;
	ImGui::SetCursorScreenPos(a);
	ImGui::InvisibleButton("##picture", ImVec2(b.x - a.x, b.y - a.y));
	if (ImGui::IsItemHovered())
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (GImGui->HoveredIdTimer > 1.2f) ImGui::SetTooltip("Click to focus and expose for this spot. Right-click to go back to the whole picture.");
	}
	if (ImGui::IsItemClicked(1))
	{
		g_app.setPoint(-1, -1);
		m_focusMarkTime = -10;
		return;
	}
	if (!ImGui::IsItemClicked(0)) return;
	const ImVec2 m = ImGui::GetIO().MousePos;
	float u = (m.x - a.x) / (b.x - a.x), v = (m.y - a.y) / (b.y - a.y);
	if (mirror) u = 1.0f - u;
	// undo the fill crop, then the rotation, to get the phone's own frame
	const float xr = u, yr = g_app.cropTop.load() + v * g_app.cropHeight.load();
	float xs = xr, ys = yr;
	switch (g_app.appliedRotation.load())
	{
	case 90: xs = yr; ys = 1.0f - xr; break;
	case 180: xs = 1.0f - xr; ys = 1.0f - yr; break;
	case 270: xs = 1.0f - yr; ys = xr; break;
	default: break;
	}
	g_app.setPoint(std::min(1.0f, std::max(0.0f, xs)), std::min(1.0f, std::max(0.0f, ys)));
	m_focusMark = m;
	m_focusMarkTime = ImGui::GetTime();
}

// ------------------------------------------------------------------ overlays
void Ui::updateOverlay(const uint8_t* data, int w, int h, int stride)
{
	const bool zebra = g_app.showZebra, peak = g_app.showPeaking;
	if (!zebra && !peak)
	{
		m_ovValid = false;
		return;
	}
	const int ow = w / 2, oh = h / 2;
	if (ow < 4 || oh < 4) return;
	m_ovLuma.resize((size_t)ow * oh);
	m_ovPixels.assign((size_t)ow * oh, 0);
	for (int y = 0; y < oh; y++)
	{
		const uint8_t* row = data + (size_t)(y * 2) * stride;
		uint8_t* l = &m_ovLuma[(size_t)y * ow];
		for (int x = 0; x < ow; x++)
		{
			const uint8_t* q = row + (size_t)x * 6; // BGR, every second pixel
			l[x] = (uint8_t)((q[0] * 29 + q[1] * 150 + q[2] * 77) >> 8);
		}
	}
	if (zebra)
	{
		const int phase = (int)(ImGui::GetTime() * 10.0) % 8;
		for (int y = 0; y < oh; y++)
			for (int x = 0; x < ow; x++)
				if (m_ovLuma[(size_t)y * ow + x] >= 242)
					m_ovPixels[(size_t)y * ow + x] = (((x + y + phase) >> 2) & 1) ? IM_COL32(255, 255, 255, 170) : IM_COL32(0, 0, 0, 170);
	}
	if (peak)
	{
		for (int y = 1; y < oh - 1; y++)
		{
			const uint8_t* up = &m_ovLuma[(size_t)(y - 1) * ow];
			const uint8_t* mid = &m_ovLuma[(size_t)y * ow];
			const uint8_t* dn = &m_ovLuma[(size_t)(y + 1) * ow];
			for (int x = 1; x < ow - 1; x++)
			{
				const int g = std::abs(mid[x + 1] - mid[x - 1]) + std::abs(dn[x] - up[x]);
				if (g > 64) m_ovPixels[(size_t)y * ow + x] = IM_COL32(255, 60, 60, 240);
			}
		}
	}
	glBindTexture(GL_TEXTURE_2D, m_ovTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (ow != m_ovW || oh != m_ovH)
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_ovPixels.data());
		m_ovW = ow;
		m_ovH = oh;
	}
	else
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, m_ovPixels.data());
	}
	m_ovValid = true;
}

void Ui::drawOverlays(const ImVec2& a, const ImVec2& b, bool mirror)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float w = b.x - a.x, h = b.y - a.y;

	if (m_ovValid && (g_app.showZebra || g_app.showPeaking))
	{
		const ImVec2 uv0(mirror ? 1.f : 0.f, 1.f), uv1(mirror ? 0.f : 1.f, 0.f);
		dl->AddImage((ImTextureID)(intptr_t)m_ovTex, a, b, uv0, uv1);
	}

	if (g_app.showGrid)
	{
		const ImU32 c = IM_COL32(255, 255, 255, 90);
		for (int i = 1; i < 3; i++)
		{
			dl->AddLine(ImVec2(a.x + w * i / 3.0f, a.y), ImVec2(a.x + w * i / 3.0f, b.y), c, 1.0f);
			dl->AddLine(ImVec2(a.x, a.y + h * i / 3.0f), ImVec2(b.x, a.y + h * i / 3.0f), c, 1.0f);
		}
	}

	const PhoneStatus st = g_app.getStatus();
	if (g_app.showLevel && st.valid && st.rotation >= 0)
	{
		// Turning the phone clockwise turns the scene counter-clockwise in the picture.
		float deg = (float)st.level;
		if (mirror) deg = -deg;
		const float r = deg * 3.14159265f / 180.0f;
		const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.5f);
		const float len = std::min(w, h) * 0.32f;
		const bool flat = std::abs(st.level) <= 1;
		const ImU32 lc = flat ? U32(col::good, 0.95f) : IM_COL32(255, 255, 255, 200);
		const ImVec2 d(cosf(r) * len, -sinf(r) * len);
		dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), IM_COL32(0, 0, 0, 120), px(4));
		dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), lc, px(2));
		// fixed reference ticks
		dl->AddLine(ImVec2(c.x - len - px(14), c.y), ImVec2(c.x - len - px(4), c.y), IM_COL32(255, 255, 255, 160), px(2));
		dl->AddLine(ImVec2(c.x + len + px(4), c.y), ImVec2(c.x + len + px(14), c.y), IM_COL32(255, 255, 255, 160), px(2));
		if (!flat)
		{
			char t[16];
			snprintf(t, sizeof(t), "%d" U8_DEG, std::abs(st.level));
			ImGui::PushFont(m_fontSmall);
			const ImVec2 ts = ImGui::CalcTextSize(t);
			dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + px(10)), IM_COL32(255, 255, 255, 220), t);
			ImGui::PopFont();
		}
	}

	if (g_app.showHistogram)
	{
		float hist[64];
		g_app.getLuma(hist);
		const float hw = px(168), hh = px(64);
		const ImVec2 p0(b.x - hw - px(14), b.y - hh - px(14));
		dl->AddRectFilled(p0, ImVec2(p0.x + hw, p0.y + hh), IM_COL32(0, 0, 0, 150), px(8));
		const float pad = px(8), bw = (hw - pad * 2) / 64.0f;
		for (int i = 0; i < 64; i++)
		{
			const float bh = hist[i] * (hh - pad * 2);
			if (bh < 0.5f) continue;
			const float x = p0.x + pad + i * bw;
			dl->AddRectFilled(ImVec2(x, p0.y + hh - pad - bh), ImVec2(x + bw + 0.5f, p0.y + hh - pad), IM_COL32(235, 238, 242, 200));
		}
	}

	const double since = ImGui::GetTime() - m_focusMarkTime;
	if (since >= 0 && since < 1.4)
	{
		const float s = px(36) * (since < 0.15 ? 1.4f - (float)since * 2.6f : 1.0f);
		const float alpha = since > 1.0 ? (float)(1.4 - since) / 0.4f : 1.0f;
		const ImVec2 c = m_focusMark;
		dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), U32(col::warn, alpha), px(4), ImDrawCornerFlags_All, px(2));
	}
}

// ------------------------------------------------------------------ the strip
void Ui::drawProStrip(const ImVec2& pos, const ImVec2& size)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), U32(col::panel), px(12));
	const PhoneCaps caps = g_app.getCaps();
	if (!caps.valid)
	{
		const char* msg = g_app.phoneTalksBack ? "Reading the phone's camera..." :
			"Pro controls (ISO, shutter, white balance, focus...) need the newest OpenCamera app on the phone.";
		ImGui::PushFont(m_fontSmall);
		const ImVec2 ts = ImGui::CalcTextSize(msg);
		dl->AddText(ImVec2(pos.x + std::max(px(16), (size.x - ts.x) * 0.5f), pos.y + (size.y - ts.y) * 0.5f), U32(col::muted), msg);
		ImGui::PopFont();
		return;
	}
	const PhoneStatus st = g_app.getStatus();
	ProSettings p = g_app.getPro();
	const bool fullManual = caps.manual && p.iso > 0 && p.expNs > 0;

	struct Tile { const char* label; std::string value; std::string sub; bool manual; bool enabled; bool on; };
	Tile t[P_COUNT];
	t[P_ISO] = { "ISO", p.iso ? std::to_string(p.iso) : "AUTO", (!p.iso && st.iso > 0) ? std::to_string(st.iso) : "", p.iso > 0, caps.manual, false };
	t[P_SHUTTER] = { "SHUTTER", p.expNs ? ShutterText(p.expNs) : "AUTO", (!p.expNs && st.expNs > 0) ? ShutterText(st.expNs) : "", p.expNs > 0, caps.manual, false };
	t[P_EV] = { "EV", EvText(p.ev, caps.evStep), "", p.ev != 0, !fullManual && caps.evMax > caps.evMin, false };
	{
		std::string wb = p.wb == 6 ? std::to_string(p.kelvin) + "K" : kWbShort[std::min(std::max(p.wb, 0), 5)];
		t[P_WB] = { "WB", wb, p.wb == 0 && p.awbLock ? "locked" : "", p.wb != 0, true, false };
	}
	t[P_FOCUS] = { "FOCUS", p.manualFocus ? "MF" : (p.afLock ? "AF-L" : "AF-C"), st.focus >= 0 ? FocusText(st.focus) : "",
	               p.manualFocus || p.afLock, caps.minFocus > 0, false };
	{
		char z[16];
		snprintf(z, sizeof(z), "%.1fx", p.zoom);
		t[P_ZOOM] = { "ZOOM", z, "", std::fabs(p.zoom - 1.0f) > 0.01f, caps.zoomMax > caps.zoomMin + 0.01f, false };
	}
	t[P_AEL] = { "AE-L", p.aeLock ? "ON" : "OFF", "", p.aeLock, !fullManual, p.aeLock };
	t[P_LIGHT] = { "LIGHT", p.torch ? "ON" : "OFF", "", p.torch, caps.torch, p.torch };
	t[P_MORE] = { "MORE", "...", "", false, true, false };

	const float pad = px(6), gap = px(6);
	const float tw = (size.x - pad * 2 - gap * (P_COUNT - 1)) / P_COUNT;
	const float th = size.y - pad * 2;
	for (int i = 0; i < P_COUNT; i++)
	{
		const ImVec2 a(pos.x + pad + i * (tw + gap), pos.y + pad);
		const ImVec2 b(a.x + tw, a.y + th);
		ImGui::SetCursorScreenPos(a);
		ImGui::PushID(i);
		const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tw, th)) && t[i].enabled;
		const bool hovered = ImGui::IsItemHovered() && t[i].enabled;
		ImGui::PopID();
		const bool open = m_proOpen == i;
		const float alpha = t[i].enabled ? 1.0f : 0.35f;
		ImU32 bgc = t[i].on ? U32(col::accent, 0.95f) : (open ? U32(col::border) : (hovered ? U32(col::panel2) : 0));
		if (bgc) dl->AddRectFilled(a, b, bgc, px(9));
		if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const ImU32 dark = IM_COL32(10, 16, 28, 255);
		ImGui::PushFont(m_fontSmall);
		const ImVec2 ls = ImGui::CalcTextSize(t[i].label);
		dl->AddText(ImVec2(a.x + (tw - ls.x) * 0.5f, a.y + px(5)), t[i].on ? dark : U32(col::muted, alpha), t[i].label);
		ImGui::PopFont();
		ImGui::PushFont(m_fontBody);
		const ImVec2 vs = ImGui::CalcTextSize(t[i].value.c_str());
		const ImU32 vc = t[i].on ? dark : (t[i].manual ? U32(col::warn, alpha) : U32(col::text, alpha));
		dl->AddText(ImVec2(a.x + (tw - vs.x) * 0.5f, a.y + px(22)), vc, t[i].value.c_str());
		ImGui::PopFont();
		if (!t[i].sub.empty())
		{
			ImGui::PushFont(m_fontSmall);
			const ImVec2 ss = ImGui::CalcTextSize(t[i].sub.c_str());
			dl->AddText(ImVec2(a.x + (tw - ss.x) * 0.5f, a.y + px(42)), t[i].on ? dark : U32(col::muted, alpha), t[i].sub.c_str());
			ImGui::PopFont();
		}
		if (!clicked) continue;
		if (i == P_AEL)
		{
			p.aeLock = !p.aeLock;
			g_app.setPro(p);
		}
		else if (i == P_LIGHT)
		{
			p.torch = !p.torch;
			g_app.setPro(p);
		}
		else
		{
			m_proOpen = i;
			m_proAnchorX = a.x;
			m_proAnchorY = a.y;
			ImGui::OpenPopup("##pro");
		}
	}
}

// ------------------------------------------------------------------ the panels
void Ui::drawProPopup()
{
	if (m_proOpen < 0) return;
	const float W = px(404);
	ImGuiIO& io = ImGui::GetIO();
	const float x = std::max(px(12), std::min(m_proAnchorX, io.DisplaySize.x - W - px(12)));
	ImGui::SetNextWindowPos(ImVec2(x, m_proAnchorY - px(10)), ImGuiCond_Always, ImVec2(0, 1));
	ImGui::SetNextWindowSize(ImVec2(W, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6), px(8)));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, col::panel);
	if (!ImGui::BeginPopup("##pro"))
	{
		m_proOpen = -1;
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		return;
	}
	ImGui::PushFont(m_fontBody);
	const PhoneCaps caps = g_app.getCaps();
	const PhoneStatus st = g_app.getStatus();
	ProSettings p = g_app.getPro();
	const ProSettings before = p;
	const int fps = std::max(1, g_app.frameRate.load());

	auto note = [&](const ImVec4& c, const char* text) {
		ImGui::PushFont(m_fontSmall);
		ImGui::PushTextWrapPos(0);
		ImGui::TextColored(c, "%s", text);
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	};

	switch (m_proOpen)
	{
	case P_ISO:
	{
		popupTitle("ISO", "How sensitive the camera is to light. Lower looks cleaner, higher is brighter but grainier.");
		std::vector<std::string> labels = { "Auto" };
		std::vector<int> values = { 0 };
		for (int v : kIsoStops)
			if (v >= caps.isoMin && v <= caps.isoMax) { labels.push_back(std::to_string(v)); values.push_back(v); }
		int sel = (int)(std::find(values.begin(), values.end(), p.iso) - values.begin());
		const int c = chipGrid(labels, sel < (int)values.size() ? sel : -1, caps.manual);
		if (c >= 0) p.iso = values[c];
		if (!caps.manual) note(col::warn, "This phone only allows automatic exposure.");
		else if (p.iso == 0 && p.expNs > 0) note(col::muted, "Auto ISO follows the scene while the shutter is set by hand.");
		else if (p.iso > 0 && p.expNs == 0) note(col::muted, "The shutter adjusts itself to keep the picture bright.");
		break;
	}
	case P_SHUTTER:
	{
		popupTitle("Shutter speed", "How long each frame is exposed. 1/50 (or 1/60) stops flicker under lamps; faster freezes motion.");
		std::vector<std::string> labels = { "Auto" };
		std::vector<int64_t> values = { 0 };
		for (int den : kShutterStops)
		{
			const int64_t ns = (int64_t)(1e9 / den + 0.5);
			if (den < fps) continue; // slower than one frame would lower the frame rate
			if (ns < caps.expMin || ns > caps.expMax) continue;
			labels.push_back("1/" + std::to_string(den));
			values.push_back(ns);
		}
		int sel = -1;
		for (size_t i = 0; i < values.size(); i++)
			if (values[i] == p.expNs || (p.expNs && std::llabs(values[i] - p.expNs) < values[i] / 50)) sel = (int)i;
		const int c = chipGrid(labels, sel, caps.manual);
		if (c >= 0) p.expNs = values[c];
		char hint[96];
		snprintf(hint, sizeof(hint), "The slowest choice is 1/%d, one frame at %d fps.", fps, fps);
		note(col::muted, hint);
		break;
	}
	case P_EV:
	{
		popupTitle("Exposure compensation", "Makes the automatic exposure brighter (+) or darker (-).");
		const float step = caps.evStep > 0 ? caps.evStep : 1.0f / 3.0f;
		ImGui::SetNextItemWidth(-1);
		int ev = p.ev;
		const std::string txt = EvText(ev, step) + " EV";
		if (ImGui::SliderInt("##ev", &ev, caps.evMin, caps.evMax, txt.c_str())) p.ev = ev;
		if (chip("Reset", false)) p.ev = 0;
		ImGui::SameLine(0, px(6));
		if (chip(p.aeLock ? "Exposure locked" : "Lock exposure (AE-L)", p.aeLock)) p.aeLock = !p.aeLock;
		break;
	}
	case P_WB:
	{
		popupTitle("White balance", "Makes white look white under different lights.");
		std::vector<std::string> labels(kWbNames, kWbNames + 7);
		const int c = chipGrid(labels, p.wb);
		if (c >= 0) p.wb = c;
		if (p.wb == 6)
		{
			ImGui::SetNextItemWidth(-1);
			int k = p.kelvin;
			char fmt[32];
			snprintf(fmt, sizeof(fmt), "%dK", k);
			if (ImGui::SliderInt("##kelvin", &k, 2300, 10000, fmt)) p.kelvin = (k + 50) / 100 * 100;
			const int presets[] = { 3200, 4300, 5600, 6500 };
			std::vector<std::string> pl;
			int psel = -1;
			for (int i = 0; i < 4; i++)
			{
				pl.push_back(std::to_string(presets[i]) + "K");
				if (presets[i] == p.kelvin) psel = i;
			}
			const int pc = chipGrid(pl, psel);
			if (pc >= 0) p.kelvin = presets[pc];
			note(col::muted, caps.kelvin ? "Lower = warmer light (lamps), higher = cooler light (shade, overcast)."
			                             : "This phone has no exact Kelvin control, so it uses the nearest preset.");
		}
		else if (p.wb == 0)
		{
			if (chip(p.awbLock ? "White balance locked" : "Lock white balance (AWB-L)", p.awbLock)) p.awbLock = !p.awbLock;
		}
		break;
	}
	case P_FOCUS:
	{
		popupTitle("Focus", "Auto keeps refocusing. Manual stays where you put it.");
		static const char* modes[] = { "Auto (AF-C)", "Manual (MF)" };
		int m = p.manualFocus ? 1 : 0;
		if (segmented("focusmode", modes, 2, &m))
		{
			p.manualFocus = m == 1;
			if (p.manualFocus && st.focus >= 0) p.focus = std::min(caps.minFocus, st.focus); // start from where AF was
		}
		if (p.manualFocus)
		{
			// slider: 0 = closest, 1 = infinity
			float t = caps.minFocus > 0 ? 1.0f - p.focus / caps.minFocus : 1.0f;
			const std::string txt = FocusText(p.focus);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::SliderFloat("##focus", &t, 0.0f, 1.0f, txt.c_str())) p.focus = std::max(0.0f, (1.0f - t) * caps.minFocus);
			ImGui::PushFont(m_fontSmall);
			ImGui::TextColored(col::muted, "Near");
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Far (" U8_INF ")").x);
			ImGui::TextColored(col::muted, "Far (" U8_INF ")");
			ImGui::PopFont();
			if (!g_app.showPeaking && chip("Show focus peaking", false)) { g_app.showPeaking = true; g_app.saveSettings(); }
		}
		else
		{
			if (chip(p.afLock ? "Focus locked" : "Lock focus (AF-L)", p.afLock)) p.afLock = !p.afLock;
			float px0, py0;
			g_app.getPoint(px0, py0);
			if (px0 >= 0)
			{
				ImGui::SameLine(0, px(6));
				if (chip("Clear focus spot", false)) g_app.setPoint(-1, -1);
			}
			if (caps.point) note(col::muted, "Tip: click the picture to focus on that spot.");
		}
		break;
	}
	case P_ZOOM:
	{
		popupTitle("Zoom", caps.zoomMin < 0.99f ? "Below 1x uses the wide lens. High zoom gets softer." : "High zoom gets softer: it enlarges the picture.");
		ImGui::SetNextItemWidth(-1);
		float z = p.zoom;
		if (ImGui::SliderFloat("##zoom", &z, caps.zoomMin, caps.zoomMax, "%.1fx", ImGuiSliderFlags_Logarithmic))
			p.zoom = std::round(z * 10.0f) / 10.0f;
		std::vector<float> quick;
		if (caps.zoomMin < 0.99f) quick.push_back(std::ceil(caps.zoomMin * 10.0f) / 10.0f);
		for (float q : { 1.0f, 2.0f, 4.0f, 8.0f })
			if (q >= caps.zoomMin - 0.01f && q <= caps.zoomMax + 0.01f) quick.push_back(q);
		std::vector<std::string> ql;
		int qsel = -1;
		for (size_t i = 0; i < quick.size(); i++)
		{
			char b[16];
			snprintf(b, sizeof(b), "%gx", quick[i]);
			ql.push_back(b);
			if (std::fabs(quick[i] - p.zoom) < 0.05f) qsel = (int)i;
		}
		const int qc = chipGrid(ql, qsel);
		if (qc >= 0) p.zoom = quick[qc];
		break;
	}
	case P_MORE:
	{
		popupTitle("More camera settings", nullptr);
		if (caps.ois)
		{
			bool v = p.ois;
			if (toggle("Optical stabilisation", &v, "Steadies small hand shake. No delay.")) p.ois = v;
		}
		if (caps.eis)
		{
			bool v = p.eis;
			if (toggle("Digital stabilisation", &v, "Steadier when walking; crops a little and adds delay.")) p.eis = v;
		}
		if (!caps.ois && !caps.eis) note(col::muted, "This camera has no stabilisation.");
		ImGui::Dummy(ImVec2(0, px(2)));
		ImGui::PushFont(m_fontSmall);
		ImGui::TextColored(col::muted, "Anti-flicker (mains frequency of your lights)");
		ImGui::PopFont();
		static const char* fl[] = { "Auto", "50 Hz", "60 Hz", "Off" };
		int f = p.flicker;
		if (segmented("flicker", fl, 4, &f)) p.flicker = f;
		ImGui::Dummy(ImVec2(0, px(2)));
		if (primaryButton("Reset all to Auto", ImVec2(ImGui::GetContentRegionAvail().x, px(34)), U32(col::muted, 0.8f)))
		{
			const bool torch = p.torch;
			p = ProSettings();
			p.torch = torch;
			g_app.setPoint(-1, -1);
		}
		break;
	}
	default:
		break;
	}

	if (!SameSettings(p, before))
	{
		g_app.setPro(p);
		g_app.saveSettings();
	}
	ImGui::PopFont();
	ImGui::EndPopup();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}
