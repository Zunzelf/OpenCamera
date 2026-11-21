#include "Ui.hpp"
#include "AppState.hpp"
#include "PhoneLink.hpp"

#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl.h>
#include <imgui_internal.h>

#include <Windows.h>
#include <shellapi.h>

#include <cmath>

#include "UiTheme.hpp"

// Virtual camera CLSID (see EncomCamera/Dll.cpp)
static const char* kVcamKey = "CLSID\\{8E14549A-DB61-4309-AFA1-3578E927E933}\\InprocServer32";

void Ui::fatal(const std::string& message)
{
	MessageBoxA(NULL, message.c_str(), "OpenCamera", MB_OK | MB_ICONERROR);
}

bool Ui::init()
{
	SetProcessDPIAware();
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fatal(std::string("Could not start the window system.\n\n") + SDL_GetError());
		return false;
	}
	float ddpi = 96.0f;
	if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
		m_scale = std::max(1.0f, ddpi / 96.0f);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	m_window = SDL_CreateWindow("OpenCamera", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		(int)px(1040), (int)px(700), SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!m_window)
	{
		fatal(std::string("Could not open the OpenCamera window.\n\n") + SDL_GetError());
		return false;
	}
	SDL_SetWindowMinimumSize(m_window, (int)px(760), (int)px(480));
	m_gl = SDL_GL_CreateContext(m_window);
	if (!m_gl)
	{
		fatal("Your graphics driver does not support OpenGL 3.0, which OpenCamera needs to draw its window.\n\nUpdating the graphics driver usually fixes this.");
		return false;
	}
	SDL_GL_MakeCurrent(m_window, m_gl);
	SDL_GL_SetSwapInterval(1);
	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK)
	{
		fatal("Could not initialise OpenGL.");
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui_ImplSDL2_InitForOpenGL(m_window, m_gl);
	ImGui_ImplOpenGL3_Init("#version 130");
	setupStyle();
	loadFonts();

	glGenTextures(1, &m_tex);
	glBindTexture(GL_TEXTURE_2D, m_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &m_ovTex);
	glBindTexture(GL_TEXTURE_2D, m_ovTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	m_pcName = PhoneLink::computerName();
	refreshEnvironment(true);
	return true;
}

void Ui::shutdown()
{
	if (m_tex) glDeleteTextures(1, &m_tex);
	if (m_ovTex) glDeleteTextures(1, &m_ovTex);
	m_tex = m_ovTex = 0;
	if (ImGui::GetCurrentContext())
	{
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();
	}
	if (m_gl) SDL_GL_DeleteContext(m_gl);
	if (m_window) SDL_DestroyWindow(m_window);
	SDL_Quit();
}

void Ui::setupStyle()
{
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 0;
	s.ChildRounding = px(12);
	s.FrameRounding = px(8);
	s.PopupRounding = px(12);
	s.GrabRounding = px(8);
	s.ScrollbarRounding = px(8);
	s.WindowBorderSize = 0;
	s.ChildBorderSize = 0;
	s.PopupBorderSize = 1;
	s.FrameBorderSize = 0;
	s.WindowPadding = ImVec2(px(20), px(20));
	s.FramePadding = ImVec2(px(12), px(8));
	s.ItemSpacing = ImVec2(px(10), px(10));
	s.ItemInnerSpacing = ImVec2(px(8), px(6));
	s.ScrollbarSize = px(10);

	ImVec4* c = s.Colors;
	c[ImGuiCol_WindowBg] = col::bg;
	c[ImGuiCol_ChildBg] = col::panel;
	c[ImGuiCol_PopupBg] = col::panel;
	c[ImGuiCol_Border] = col::border;
	c[ImGuiCol_Text] = col::text;
	c[ImGuiCol_TextDisabled] = col::muted;
	c[ImGuiCol_FrameBg] = col::panel2;
	c[ImGuiCol_FrameBgHovered] = col::border;
	c[ImGuiCol_FrameBgActive] = col::border;
	c[ImGuiCol_Button] = col::panel2;
	c[ImGuiCol_ButtonHovered] = col::border;
	c[ImGuiCol_ButtonActive] = col::border;
	c[ImGuiCol_CheckMark] = col::accent;
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = col::border;
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
	c[ImGuiCol_Separator] = col::border;
	c[ImGuiCol_SliderGrab] = col::accent;
	c[ImGuiCol_SliderGrabActive] = col::accentHi;
	s.GrabMinSize = px(14);
}

bool Ui::loadFonts()
{
	ImGuiIO& io = ImGui::GetIO();
	static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x221E, 0x221E, 0 };
	const char* regular = "C:\\Windows\\Fonts\\segoeui.ttf";
	const char* semibold = "C:\\Windows\\Fonts\\seguisb.ttf";
	ImFontConfig cfg;
	cfg.OversampleH = 2;
	m_fontBody = io.Fonts->AddFontFromFileTTF(regular, px(17), &cfg, ranges);
	if (!m_fontBody)
	{
		// Fall back to ImGui's built-in font rather than refusing to start.
		m_fontBody = m_fontSmall = m_fontTitle = m_fontHuge = io.Fonts->AddFontDefault();
		return false;
	}
	m_fontSmall = io.Fonts->AddFontFromFileTTF(regular, px(14), &cfg, ranges);
	m_fontTitle = io.Fonts->AddFontFromFileTTF(semibold, px(24), &cfg, ranges);
	m_fontHuge = io.Fonts->AddFontFromFileTTF(semibold, px(34), &cfg, ranges);
	if (!m_fontSmall) m_fontSmall = m_fontBody;
	if (!m_fontTitle) m_fontTitle = m_fontBody;
	if (!m_fontHuge) m_fontHuge = m_fontTitle;
	return true;
}

void Ui::refreshEnvironment(bool force)
{
	const uint32_t now = SDL_GetTicks();
	if (!force && now - m_lastEnvRefresh < 3000) return;
	m_lastEnvRefresh = now;
	m_addresses = PhoneLink::localAddresses();
	if (m_ourDll64.empty())
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(NULL, exe, MAX_PATH);
		std::string dir(exe);
		m_ourDll64 = dir.substr(0, dir.find_last_of("\\/")) + "\\EncomCamerax64.dll";
	}
	// The CLSID comes from a widely copied sample, and the original OpenCamera
	// uses it too: only call it installed if it points at *our* DLL.
	char path[MAX_PATH * 2] = {};
	DWORD size = sizeof(path);
	const bool registered = RegGetValueA(HKEY_CLASSES_ROOT, kVcamKey, nullptr, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &size) == ERROR_SUCCESS;
	m_vcamInstalled = registered && _stricmp(path, m_ourDll64.c_str()) == 0;
	m_vcamOther = registered && !m_vcamInstalled;
}

void Ui::installVirtualCamera()
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	std::wstring dir(exe);
	dir = dir.substr(0, dir.find_last_of(L"\\/"));
	const std::wstring dll64 = dir + L"\\EncomCamerax64.dll";
	const std::wstring dll32 = dir + L"\\EncomCamerax86.dll";
	if (GetFileAttributesW(dll64.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		g_app.log(LogLevel::Error, "EncomCamerax64.dll is missing next to OpenCamera. Reinstall OpenCamera.");
		return;
	}
	// One administrator prompt registers both the 64-bit and 32-bit camera.
	std::wstring cmd = L"/s /c \"\"%SystemRoot%\\System32\\regsvr32.exe\" /s \"" + dll64 + L"\"";
	if (GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES)
		cmd += L" & \"%SystemRoot%\\SysWOW64\\regsvr32.exe\" /s \"" + dll32 + L"\"";
	cmd += L"\"";
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = L"cmd.exe";
	sei.lpParameters = cmd.c_str();
	sei.nShow = SW_HIDE;
	if (ShellExecuteExW(&sei))
	{
		if (sei.hProcess)
		{
			WaitForSingleObject(sei.hProcess, 15000);
			CloseHandle(sei.hProcess);
		}
		refreshEnvironment(true);
		g_app.log(m_vcamInstalled ? LogLevel::Info : LogLevel::Warning,
			m_vcamInstalled ? "Virtual camera installed. Restart Zoom/Teams/your browser to see \"OpenCamera\"." : "The virtual camera was not installed.");
	}
	else
	{
		g_app.log(LogLevel::Warning, "Installing the virtual camera was cancelled.");
	}
}

void Ui::updatePreviewTexture()
{
	if (!m_preview.isOpen()) m_preview.open();
	if (!m_preview.live())
	{
		m_texValid = false;
		return;
	}
	const int64_t counter = m_preview.frameCounter();
	if (counter == m_texCounter && m_texValid) return;
	glBindTexture(GL_TEXTURE_2D, m_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // DIB rows are DWORD aligned
	const bool ok = m_preview.readLatest([&](const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
		if ((int)w != m_texW || (int)h != m_texH)
		{
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_BGR, GL_UNSIGNED_BYTE, data);
			m_texW = (int)w;
			m_texH = (int)h;
		}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, data);
		}
		updateOverlay(data, (int)w, (int)h, (int)stride);
	});
	if (ok)
	{
		m_texCounter = counter;
		m_texValid = true;
	}
}

void Ui::run()
{
	while (!m_done)
	{
		const bool live = g_app.phase.load() == Phase::Live;
		const bool minimized = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
		// Sleep until input or the next preview frame instead of spinning.
		SDL_Event e;
		if (SDL_WaitEventTimeout(&e, minimized ? 250 : (live ? 10 : 60)))
		{
			do
			{
				ImGui_ImplSDL2_ProcessEvent(&e);
				if (e.type == SDL_QUIT) m_done = true;
				if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(m_window))
					m_done = true;
			} while (SDL_PollEvent(&e));
		}
		if (minimized) continue;
		refreshEnvironment(false);
		updatePreviewTexture();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame(m_window);
		ImGui::NewFrame();
		frame();
		ImGui::Render();
		int w, h;
		SDL_GL_GetDrawableSize(m_window, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(col::bg.x, col::bg.y, col::bg.z, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(m_window);
	}
	g_app.quitting = true;
}

// ------------------------------------------------------------------ widgets
void Ui::spinner(const ImVec2& c, float r, ImU32 color)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float t = (float)ImGui::GetTime() * 5.0f;
	const int n = 24;
	dl->PathClear();
	for (int i = 0; i <= n; i++)
	{
		const float a = t + (float)i / n * 4.2f;
		dl->PathLineTo(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r));
	}
	dl->PathStroke(color, false, px(3));
}

bool Ui::segmented(const char* id, const char* const* labels, int count, int* value)
{
	ImGui::PushID(id);
	const float full = ImGui::GetContentRegionAvail().x;
	const float h = px(36);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + full, p.y + h), U32(col::panel2), px(10));
	const float pad = px(3);
	const float w = (full - pad * 2) / count;
	bool changed = false;
	for (int i = 0; i < count; i++)
	{
		const ImVec2 a(p.x + pad + w * i, p.y + pad);
		const ImVec2 b(a.x + w, p.y + h - pad);
		ImGui::SetCursorScreenPos(a);
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("seg", ImVec2(w, h - pad * 2)) && *value != i)
		{
			*value = i;
			changed = true;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		if (*value == i) dl->AddRectFilled(a, b, U32(col::accent), px(8));
		else if (hovered) dl->AddRectFilled(a, b, U32(col::border), px(8));
		const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
		dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f),
			*value == i ? IM_COL32(10, 16, 28, 255) : U32(col::text), labels[i]);
	}
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
	ImGui::Dummy(ImVec2(full, px(4)));
	ImGui::PopID();
	return changed;
}

bool Ui::toggle(const char* label, bool* v, const char* hint)
{
	const float h = px(22), w = px(40);
	const float full = ImGui::GetContentRegionAvail().x;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	const float rowH = hint ? px(44) : px(28);
	const bool clicked = ImGui::InvisibleButton("toggle", ImVec2(full, rowH));
	ImGui::PopID();
	if (clicked) *v = !*v;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 sp(p.x + full - w, p.y + (hint ? px(4) : (rowH - h) * 0.5f));
	const float t = *v ? 1.0f : 0.0f;
	dl->AddRectFilled(sp, ImVec2(sp.x + w, sp.y + h), *v ? U32(col::accent) : U32(col::border), h * 0.5f);
	dl->AddCircleFilled(ImVec2(sp.x + h * 0.5f + t * (w - h), sp.y + h * 0.5f), h * 0.5f - px(3), IM_COL32(255, 255, 255, 255));
	dl->AddText(ImVec2(p.x, p.y + (hint ? 0 : (rowH - ImGui::GetFontSize()) * 0.5f)), U32(col::text), label);
	if (hint)
	{
		ImGui::PushFont(m_fontSmall);
		dl->AddText(m_fontSmall, m_fontSmall->FontSize, ImVec2(p.x, p.y + px(22)), U32(col::muted), hint);
		ImGui::PopFont();
	}
	return clicked;
}

bool Ui::primaryButton(const char* label, const ImVec2& size, ImU32 color)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton(label, size);
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
	if (hovered) c = ImVec4(std::min(1.f, c.x * 1.12f), std::min(1.f, c.y * 1.12f), std::min(1.f, c.z * 1.12f), c.w);
	if (held) c = ImVec4(c.x * 0.9f, c.y * 0.9f, c.z * 0.9f, c.w);
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(c), px(10));
	const char* end = ImGui::FindRenderedTextEnd(label);
	const ImVec2 ts = ImGui::CalcTextSize(label, end);
	dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), IM_COL32(10, 16, 28, 255), label, end);
	return clicked;
}

float Ui::statusPill(const ImVec2& rightTop)
{
	const Phase ph = g_app.phase.load();
	const char* text = "Waiting for phone";
	ImVec4 c = col::muted;
	switch (ph)
	{
	case Phase::Waiting: text = "Waiting for phone"; c = col::muted; break;
	case Phase::AskApproval: text = "Phone is asking to connect"; c = col::warn; break;
	case Phase::Connecting: text = "Connecting"; c = col::accent; break;
	case Phase::Live: text = "Live"; c = col::good; break;
	case Phase::Reconnecting: text = "Reconnecting"; c = col::warn; break;
	case Phase::Error: text = "Needs attention"; c = col::bad; break;
	}
	ImGui::PushFont(m_fontBody);
	const ImVec2 ts = ImGui::CalcTextSize(text);
	const float h = px(32), dot = px(5);
	const float w = ts.x + px(16) + dot * 2 + px(10) + px(14);
	const ImVec2 a(rightTop.x - w, rightTop.y), b(rightTop.x, rightTop.y + h);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, U32(c, 0.14f), h * 0.5f);
	float pulse = 1.0f;
	if (ph == Phase::Live || ph == Phase::Connecting || ph == Phase::Reconnecting)
		pulse = 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f));
	dl->AddCircleFilled(ImVec2(a.x + px(16) + dot, a.y + h * 0.5f), dot, U32(c, pulse));
	dl->AddText(ImVec2(a.x + px(16) + dot * 2 + px(10), a.y + (h - ts.y) * 0.5f), U32(c), text);
	ImGui::PopFont();
	return w;
}

// ------------------------------------------------------------------ layout
void Ui::frame()
{
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PushFont(m_fontBody);

	drawHeader();

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float sideW = px(320);
	const float gap = px(16);
	drawPreview(ImVec2(avail.x - sideW - gap, avail.y));
	ImGui::SameLine(0, gap);
	drawSidebar(ImVec2(sideW, avail.y));

	drawProPopup();
	drawApproval();
	drawActivity();

	ImGui::PopFont();
	ImGui::End();
}

void Ui::drawHeader()
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// small camera mark
	const float s = px(30);
	dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), U32(col::accent), px(9));
	dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.24f, U32(col::bg));
	dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.12f, U32(col::accent));
	ImGui::SetCursorScreenPos(ImVec2(p.x + s + px(12), p.y + (s - m_fontTitle->FontSize) * 0.5f - px(1)));
	ImGui::PushFont(m_fontTitle);
	ImGui::TextUnformatted("OpenCamera");
	ImGui::PopFont();
	ImGui::SameLine();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
	ImGui::TextColored(col::muted, "phone as webcam");
	const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
	const float pillW = statusPill(ImVec2(right, p.y - px(1)));
	const char* actLabel = "Activity log";
	const float actW = ImGui::CalcTextSize(actLabel).x + px(24);
	ImGui::SetCursorScreenPos(ImVec2(right - pillW - px(10) - actW, p.y - px(1)));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, col::muted);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(16));
	if (ImGui::Button(actLabel, ImVec2(actW, px(32)))) m_showActivity = true;
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + s + px(16)));
}

void Ui::drawPreview(const ImVec2& size)
{
	const bool live = m_texValid && g_app.phase.load() == Phase::Live;
	const float stripH = live ? px(74) : 0.0f;
	const float gap = live ? px(12) : 0.0f;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImGui::BeginGroup();
	ImGui::BeginChild("##preview", ImVec2(size.x, size.y - stripH - gap), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 inner = ImGui::GetContentRegionAvail();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	if (live)
	{
		const ImVec2 wp = ImGui::GetWindowPos();
		const ImVec2 ws = ImGui::GetWindowSize();
		dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(0, 0, 0, 255), px(12));
		// fit the picture into the whole panel
		float w = ws.x, h = ws.x * m_texH / (float)m_texW;
		if (h > ws.y) { h = ws.y; w = ws.y * m_texW / (float)m_texH; }
		const ImVec2 a(wp.x + (ws.x - w) * 0.5f, wp.y + (ws.y - h) * 0.5f);
		const ImVec2 b(a.x + w, a.y + h);
		const bool mirror = g_app.mirror;
		// texture rows are bottom-up: flip V
		const ImVec2 uv0(mirror ? 1.f : 0.f, 1.f), uv1(mirror ? 0.f : 1.f, 0.f);
		dl->AddImageRounded((ImTextureID)(intptr_t)m_tex, a, b, uv0, uv1, IM_COL32_WHITE, px(12));
		drawOverlays(a, b, mirror);

		// stats chip
		char buf[160];
		snprintf(buf, sizeof(buf), "%dx%d   %.0f fps   %.1f Mbps   %.1f ms on PC",
			g_app.videoWidth.load(), g_app.videoHeight.load(), g_app.fps.load(), g_app.kbps.load() / 1000.0f, g_app.decodeMs.load());
		ImGui::PushFont(m_fontSmall);
		const ImVec2 ts = ImGui::CalcTextSize(buf);
		const ImVec2 cp(wp.x + px(14), wp.y + ws.y - ts.y - px(26));
		dl->AddRectFilled(cp, ImVec2(cp.x + ts.x + px(20), cp.y + ts.y + px(12)), IM_COL32(0, 0, 0, 150), px(8));
		dl->AddText(ImVec2(cp.x + px(10), cp.y + px(6)), IM_COL32(255, 255, 255, 230), buf);
		ImGui::PopFont();

		// Buttons first, so they win over the click-to-focus area below them.
		drawPreviewTools(wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
		handlePreviewClick(a, b, mirror);
	}
	else
	{
		drawOnboarding(origin, inner);
	}
	ImGui::EndChild();
	if (live)
	{
		ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + size.y - stripH));
		drawProStrip(ImGui::GetCursorScreenPos(), ImVec2(size.x, stripH));
		ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + size.y - stripH));
		ImGui::Dummy(ImVec2(size.x, stripH));
	}
	ImGui::EndGroup();
}

void Ui::drawOnboarding(const ImVec2& origin, const ImVec2& size)
{
	const Phase ph = g_app.phase.load();
	std::string headline, detail;
	g_app.getMessage(headline, detail);
	ImDrawList* dl = ImGui::GetWindowDrawList();

	const float colW = std::min(size.x - px(40), px(560));
	const float x0 = origin.x + (size.x - colW) * 0.5f;
	float y = origin.y + std::max(px(24), size.y * 0.10f);

	if (ph == Phase::Connecting || ph == Phase::Reconnecting)
	{
		const ImVec2 c(origin.x + size.x * 0.5f, origin.y + size.y * 0.42f);
		spinner(c, px(22), U32(ph == Phase::Connecting ? col::accent : col::warn));
		ImGui::PushFont(m_fontTitle);
		const ImVec2 ts = ImGui::CalcTextSize(headline.c_str());
		dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + px(44)), U32(col::text), headline.c_str());
		ImGui::PopFont();
		if (!detail.empty())
		{
			ImGui::SetCursorScreenPos(ImVec2(x0, c.y + px(84)));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
			ImGui::TextColored(col::muted, "%s", detail.c_str());
			ImGui::PopTextWrapPos();
		}
		return;
	}

	ImGui::SetCursorScreenPos(ImVec2(x0, y));
	ImGui::BeginGroup(); // later lines start at x0, not the panel edge
	if (ph == Phase::Error)
	{
		ImGui::PushFont(m_fontTitle);
		ImGui::TextColored(col::bad, "%s", headline.c_str());
		ImGui::PopFont();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
		ImGui::TextColored(col::muted, "%s", detail.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		return;
	}

	ImGui::PushFont(m_fontTitle);
	ImGui::TextUnformatted("Connect your phone");
	ImGui::PopFont();
	ImGui::TextColored(col::muted, "Three steps, then pick \"OpenCamera\" as the camera in Zoom, Teams, Discord or your browser.");
	ImGui::Dummy(ImVec2(0, px(10)));

	auto step = [&](int n, const char* title, const char* sub) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float r = px(15);
		dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, U32(col::accent, 0.16f));
		char num[4];
		snprintf(num, sizeof(num), "%d", n);
		const ImVec2 ns = ImGui::CalcTextSize(num);
		dl->AddText(ImVec2(p.x + r - ns.x * 0.5f, p.y + r - ns.y * 0.5f), U32(col::accent), num);
		ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + px(14), p.y + px(4)));
		ImGui::BeginGroup();
		ImGui::TextUnformatted(title);
		if (sub)
		{
			ImGui::PushFont(m_fontSmall);
			ImGui::PushTextWrapPos(x0 + colW - ImGui::GetWindowPos().x);
			ImGui::TextColored(col::muted, "%s", sub);
			ImGui::PopTextWrapPos();
			ImGui::PopFont();
		}
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y + px(10)));
	};
	step(1, "Open the OpenCamera app on your Android phone", nullptr);
	step(2, "Put the phone on the same Wi-Fi as this PC, or plug it in", "USB cable: turn on USB debugging on the phone. A phone hotspot works too.");
	step(3, "Tap this computer in the app's list", nullptr);

	// The PC's name as it appears on the phone, plus the address fallback.
	const ImVec2 cardPos(x0, ImGui::GetCursorScreenPos().y + px(4));
	const float cardH = px(132);
	dl->AddRectFilled(cardPos, ImVec2(cardPos.x + colW, cardPos.y + cardH), U32(col::panel2), px(12));
	ImGui::SetCursorScreenPos(ImVec2(cardPos.x + px(18), cardPos.y + px(14)));
	ImGui::PushFont(m_fontSmall);
	ImGui::TextColored(col::muted, "THIS COMPUTER");
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(ImVec2(cardPos.x + px(18), ImGui::GetCursorScreenPos().y - px(4)));
	ImGui::PushFont(m_fontHuge);
	ImGui::TextUnformatted(m_pcName.c_str());
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(ImVec2(cardPos.x + px(18), ImGui::GetCursorScreenPos().y));
	ImGui::PushFont(m_fontSmall);
	if (m_addresses.empty())
	{
		ImGui::TextColored(col::warn, "This PC is not on a network. Connect it to Wi-Fi or Ethernet.");
	}
	else
	{
		std::string list = "Not in the list? Type ";
		for (size_t i = 0; i < m_addresses.size() && i < 3; i++)
		{
			if (i) list += (i + 1 == std::min<size_t>(m_addresses.size(), 3)) ? " or " : ", ";
			list += m_addresses[i];
		}
		list += " in the app.";
		ImGui::PushTextWrapPos(cardPos.x + colW - px(18) - ImGui::GetWindowPos().x);
		ImGui::TextColored(col::muted, "%s", list.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(ImVec2(x0, cardPos.y + cardH + px(14)));

	ImGui::PushFont(m_fontSmall);
	ImGui::PushTextWrapPos(x0 + colW - ImGui::GetWindowPos().x);
	ImGui::TextColored(col::muted, "If the phone can't find this PC, allow OpenCamera in the Windows Firewall prompt (Private networks).");
	ImGui::PopTextWrapPos();
	ImGui::PopFont();
	ImGui::EndGroup();
}

void Ui::drawSidebar(const ImVec2& size)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, col::bg);
	ImGui::BeginChild("##side", size, false); // scrolls only if the window is very short
	ImGui::PopStyleColor();
	const Phase ph = g_app.phase.load();
	const bool connected = ph == Phase::Live || ph == Phase::Connecting || ph == Phase::Reconnecting;

	auto section = [&](const char* title) {
		ImGui::PushFont(m_fontSmall);
		ImGui::TextColored(col::muted, "%s", title);
		ImGui::PopFont();
	};
	auto card = [&](const char* id, float h, auto&& body) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(14)));
		ImGui::BeginChild(id, ImVec2(0, h), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
		body();
		ImGui::EndChild();
		ImGui::PopStyleVar();
	};

	// Phone
	std::string name, addr;
	g_app.getPhone(name, addr);
	const PhoneStatus pst = g_app.getStatus();
	const bool showBattery = connected && pst.valid && pst.battery >= 0;
	const bool phoneHot = showBattery && pst.temperature >= 42;
	card("##phone", connected ? (showBattery ? (phoneHot ? (pst.charging ? px(182) : px(164)) : px(146)) : px(122)) : px(78), [&]() {
		section("PHONE");
		if (connected)
		{
			ImGui::TextUnformatted(name.empty() ? "Phone" : name.c_str());
			ImGui::SameLine();
			ImGui::TextColored(col::muted, "%s", addr.c_str());
			// How the phone is connected: a small badge on the right.
			static const char* kinds[] = { nullptr, "Wi-Fi", "USB cable", "Network cable", "This PC" };
			const int kind = g_app.linkKind.load();
			if (kind > 0 && kind < 5)
			{
				ImGui::SameLine();
				ImGui::PushFont(m_fontSmall);
				const ImVec2 ts = ImGui::CalcTextSize(kinds[kind]);
				const float bw = ts.x + px(16), bh = ts.y + px(6);
				const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
				const ImVec2 cur = ImGui::GetCursorScreenPos();
				const ImVec2 p0(std::max(cur.x, right - bw), cur.y);
				const ImVec4 tint = kind == 2 ? col::good : col::accent;
				ImDrawList* bdl = ImGui::GetWindowDrawList();
				bdl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh), U32(tint, 0.18f), bh * 0.5f);
				bdl->AddText(ImVec2(p0.x + px(8), p0.y + px(3)), U32(tint), kinds[kind]);
				ImGui::Dummy(ImVec2(0, 0));
				ImGui::PopFont();
			}
			if (showBattery)
			{
				ImGui::PushFont(m_fontSmall);
				const bool hot = pst.temperature >= 42;
				const bool low = pst.battery <= 15 && !pst.charging;
				if (hot)
				{
					ImGui::PushTextWrapPos(0);
					if (pst.charging)
						ImGui::TextColored(col::warn, "Phone is warm (%d" U8_DEG "C) while charging. Charging adds heat: lower the quality, turn off its preview, or take off its case.", pst.temperature);
					else
						ImGui::TextColored(col::warn, "Phone is warm (%d" U8_DEG "C). Lower the quality or turn off its preview.", pst.temperature);
					ImGui::PopTextWrapPos();
				}
				else
					ImGui::TextColored(low ? col::warn : col::muted, "Battery %d%%%s   " U8_DOT "   %d" U8_DEG "C",
						pst.battery, pst.charging ? " (charging)" : "", pst.temperature);
				ImGui::PopFont();
			}
			if (primaryButton("Disconnect", ImVec2(ImGui::GetContentRegionAvail().x, px(34)), U32(col::bad, 0.9f)))
				g_app.requestDisconnect = true;
		}
		else
		{
			ImGui::TextColored(col::muted, "Not connected");
		}
	});
	ImGui::Dummy(ImVec2(0, px(2)));

	// Camera and quality
	card("##camera", px(254), [&]() {
		section("CAMERA");
		static const char* cams[] = { "Front", "Back" };
		int cam = g_app.frontCamera ? 0 : 1;
		if (segmented("cam", cams, 2, &cam))
		{
			g_app.frontCamera = cam == 0;
			g_app.requestRestartVideo = true;
			g_app.saveSettings();
		}
		section("QUALITY");
		const char* q[kQualityPresetCount];
		for (int i = 0; i < kQualityPresetCount; i++) q[i] = kQualityPresets[i].label;
		int qi = g_app.quality;
		if (segmented("quality", q, kQualityPresetCount, &qi))
		{
			g_app.quality = qi;
			g_app.requestRestartVideo = true;
			g_app.saveSettings();
		}
		section("FRAME RATE");
		// Rates the phone offers at this size (from the phone), else common ones.
		std::vector<int> rates;
		const PhoneCaps caps = g_app.getCaps();
		const int height = kQualityPresets[g_app.quality.load()].height;
		for (const auto& row : caps.fps)
			if (row.first == height)
				for (int r : row.second)
					if (r == 24 || r == 25 || r == 30 || r == 48 || r == 50 || r == 60) rates.push_back(r);
		if (rates.empty()) rates = { 24, 30, 60 };
		std::vector<std::string> labels;
		std::vector<const char*> ptrs;
		int sel = -1;
		for (size_t i = 0; i < rates.size(); i++)
		{
			labels.push_back(std::to_string(rates[i]));
			if (rates[i] == g_app.frameRate.load()) sel = (int)i;
		}
		for (const auto& l : labels) ptrs.push_back(l.c_str());
		int fi = sel;
		if (segmented("fps", ptrs.data(), (int)ptrs.size(), &fi) && fi >= 0)
		{
			g_app.frameRate = rates[fi];
			g_app.requestRestartVideo = true;
			g_app.saveSettings();
		}
	});
	ImGui::Dummy(ImVec2(0, px(2)));

	// Picture
	card("##picture", px(132), [&]() {
		bool mirror = g_app.mirror;
		if (toggle("Mirror image", &mirror, "Flip left and right"))
		{
			g_app.mirror = mirror;
			g_app.saveSettings();
		}
		bool gpu = g_app.gpuDecode;
		const char* hint = (ph == Phase::Live) ? (g_app.hwDecodeActive ? "Active: graphics card decodes the video" : "Active: processor decodes the video")
		                                      : "Lower CPU use, about 2-5 ms more delay";
		if (toggle("GPU decoding", &gpu, hint))
		{
			g_app.gpuDecode = gpu;
			g_app.requestRestartVideo = true;
			g_app.saveSettings();
		}
	});
	ImGui::Dummy(ImVec2(0, px(2)));

	// Virtual camera install state
	card("##vcam", m_vcamInstalled ? px(84) : std::max(px(110), std::min(ImGui::GetContentRegionAvail().y, px(132))), [&]() {
		section("VIRTUAL CAMERA");
		ImGui::PushFont(m_fontSmall);
		ImGui::PushTextWrapPos(0);
		if (m_vcamInstalled)
			ImGui::TextColored(col::muted, "Installed. Choose \"OpenCamera\" as the camera in your video app.");
		else if (m_vcamOther)
			ImGui::TextColored(col::warn, "An older OpenCamera camera is installed. Update it so video apps get this picture.");
		else
			ImGui::TextColored(col::warn, "Not installed yet. Video apps can't see OpenCamera until it is.");
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
		if (!m_vcamInstalled && primaryButton(m_vcamOther ? "Update virtual camera" : "Install virtual camera",
		                                      ImVec2(ImGui::GetContentRegionAvail().x, px(34)), U32(col::accent)))
			installVirtualCamera();
	});

	ImGui::EndChild();
}

void Ui::drawApproval()
{
	static bool open = false;
	const bool asking = g_app.phase.load() == Phase::AskApproval && g_app.approvalAnswer.load() == 0;
	if (asking && !open)
	{
		ImGui::OpenPopup("Allow this phone?");
		open = true;
		SDL_RaiseWindow(m_window);
	}
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(px(420), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(22)));
	if (ImGui::BeginPopupModal("Allow this phone?", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		std::string name, addr;
		g_app.getPhone(name, addr);
		ImGui::PushFont(m_fontTitle);
		ImGui::TextUnformatted("Allow this phone?");
		ImGui::PopFont();
		ImGui::PushTextWrapPos(0);
		ImGui::TextColored(col::muted, "%s wants to be this computer's webcam.", name.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, px(4)));
		bool remember = g_app.approvalRemember;
		if (toggle("Always allow this phone", &remember)) g_app.approvalRemember = remember;
		ImGui::Dummy(ImVec2(0, px(6)));
		const float bw = (ImGui::GetContentRegionAvail().x - px(10)) * 0.5f;
		if (primaryButton("Allow", ImVec2(bw, px(40)), U32(col::accent)))
		{
			g_app.approvalAnswer = 1;
		}
		ImGui::SameLine(0, px(10));
		if (primaryButton("Block", ImVec2(bw, px(40)), U32(col::muted, 0.8f)))
		{
			g_app.approvalAnswer = 2;
		}
		if (!asking)
		{
			ImGui::CloseCurrentPopup();
			open = false;
		}
		ImGui::EndPopup();
	}
	else
	{
		open = false;
	}
	ImGui::PopStyleVar();
}

void Ui::drawActivity()
{
	if (!m_showActivity) return;
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(px(620), px(400)), ImGuiCond_Appearing);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, col::panel);
	ImGui::PushStyleColor(ImGuiCol_TitleBg, col::panel2);
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, col::panel2);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(12));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (ImGui::Begin("Activity log", &m_showActivity, ImGuiWindowFlags_NoCollapse))
	{
		if (ImGui::Button("Clear")) g_app.clearLog();
		ImGui::SameLine();
		ImGui::TextColored(col::muted, "What OpenCamera has been doing, newest at the bottom.");
		ImGui::BeginChild("##log", ImVec2(0, 0), false);
		ImGui::PushFont(m_fontSmall);
		for (const LogLine& l : g_app.logSnapshot())
		{
			const ImVec4 c = l.level == LogLevel::Error ? col::bad : (l.level == LogLevel::Warning ? col::warn : col::text);
			ImGui::TextColored(col::muted, "%s", l.time.c_str());
			ImGui::SameLine();
			ImGui::PushTextWrapPos(0);
			ImGui::TextColored(c, "%s", l.text.c_str());
			ImGui::PopTextWrapPos();
		}
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(20)) ImGui::SetScrollHereY(1.0f);
		ImGui::PopFont();
		ImGui::EndChild();
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(3);
}
