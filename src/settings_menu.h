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

/* Anti-aliasing: 0 off, 1 the game's own SPU MLAA (slower: two full-frame
 * readbacks a frame, and the game drops to its 30 FPS mode). Read per frame
 * by src/tj_mlaa.cpp, so the menu switches it live. */
enum { TJ_AA_OFF = 0, TJ_AA_GAME_MLAA = 1 };
int tj_settings_anti_aliasing(void);

/* "Quit game" was chosen in the menu: take the same path as closing the window. */
bool tj_settings_quit_requested(void);
