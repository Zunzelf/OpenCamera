#include "AppState.hpp"
#include "PhoneLink.hpp"
#include "Ui.hpp"

int main(int, char**)
{
	g_app.loadSettings();

	Ui ui;
	if (!ui.init())
	{
		ui.shutdown();
		return 1;
	}

	// Network problems are shown inside the window, so the UI starts anyway.
	PhoneLink link;
	link.start();

	ui.run();

	g_app.saveSettings();
	link.stop();
	ui.shutdown();
	return 0;
}
