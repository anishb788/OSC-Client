#include <SDL.h>

#include "App.h"

int main(int argc, char* argv[]) {
	App app;
	if (!app.init(argc, argv)) return 1;
	app.run();
	app.shutdown();
	return 0;
}
