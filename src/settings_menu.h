/*
 * Tokyo Jungle runner -- settings and the in-game settings menu.
 *
 * Settings the port itself has (none of them is the game's): how the window
 * is shown (windowed, borderless fullscreen, exclusive fullscreen), its size
 * or the display mode, the internal render resolution, the aspect ratio,
 * vsync and the fps readout. They live in settings.ini beside the
 * executable. The menu opens with Escape or the controller's Guide button
 * (or Back + Start together) and draws over the game through the Vulkan
 * backend's UI image; while it is open the game sees its pad at rest.
 *
 * Every function here runs on the frame-clock thread (the one that opened
 * the window and presents), except tj_settings_load, which main() calls
 * before that thread starts.
 */
#pragma once

/* Read settings.ini (or $TJ_SETTINGS) and the environment overrides. A
 * headless run reads no file: scripted tests keep the defaults. */
void tj_settings_load(void);

/* The backend is up: apply what needs the window or the device. */
void tj_settings_backend_ready(void);

/* Once per frame-clock wake: input, applying changes, redrawing the menu. */
void tj_settings_frame(void);

/* The fps readout (F1 or the menu toggles it). */
bool tj_settings_show_fps(void);

/* "Quit game" was chosen in the menu: take the same path as closing the window. */
bool tj_settings_quit_requested(void);
