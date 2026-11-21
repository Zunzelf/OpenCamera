#pragma once

#include <GL/glew.h>
#include <SDL.h>
#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../shared/FrameShm.h"

class Ui
{
public:
	bool init();
	void run();
	void shutdown();

	static void fatal(const std::string& message);

private:
	void setupStyle();
	bool loadFonts();
	void frame();
	void drawHeader();
	void drawPreview(const ImVec2& size);
	void drawOnboarding(const ImVec2& origin, const ImVec2& size);
	void drawSidebar(const ImVec2& size);
	void drawApproval();
	void drawActivity();
	void updatePreviewTexture();
	void refreshEnvironment(bool force);
	void installVirtualCamera();

	bool segmented(const char* id, const char* const* labels, int count, int* value);
	bool toggle(const char* label, bool* v, const char* hint = nullptr);
	bool primaryButton(const char* label, const ImVec2& size, ImU32 color);
	void spinner(const ImVec2& center, float radius, ImU32 color);
	void statusPill(const ImVec2& pos);

	float px(float v) const { return v * m_scale; }

	SDL_Window* m_window = nullptr;
	SDL_GLContext m_gl = nullptr;
	float m_scale = 1.0f;

	ImFont* m_fontBody = nullptr;
	ImFont* m_fontSmall = nullptr;
	ImFont* m_fontTitle = nullptr;
	ImFont* m_fontHuge = nullptr;

	GLuint m_tex = 0;
	int m_texW = 0, m_texH = 0;
	int64_t m_texCounter = -1;
	bool m_texValid = false;
	ocam::FrameReader m_preview;

	bool m_showActivity = false;
	bool m_vcamInstalled = false;
	bool m_vcam32Installed = false;
	std::vector<std::string> m_addresses;
	std::string m_pcName;
	uint32_t m_lastEnvRefresh = 0;
	bool m_done = false;
};
