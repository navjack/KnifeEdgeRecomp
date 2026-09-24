#ifndef KNIFE_EDGE_MACOS_MENU_H
#define KNIFE_EDGE_MACOS_MENU_H

/* Installs the macOS application menu. SDL3 does not create one, which leaves
 * the app with no menu bar, nothing for the system bar to show, and no Quit
 * item -- so Command-Q never becomes an action and never reaches SDL.
 *
 * Call after SDL_Init(SDL_INIT_VIDEO). Does nothing if a menu already exists. */
void knife_edge_install_macos_menu(const char* application_name);

#endif /* KNIFE_EDGE_MACOS_MENU_H */
