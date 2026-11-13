#ifndef IGGLE_INPUT_H
#define IGGLE_INPUT_H
#include <windows.h>
#define IGGLE_INPUT_TIMER 0x4947
void iggle_input_init(HWND window);
void iggle_input_poll(HWND window);
void iggle_input_release(HWND window);
void iggle_input_set_gameplay(BOOL active);
BOOL iggle_input_message(HWND window, UINT *message, WPARAM *key, LPARAM *details);
SHORT iggle_input_key(int key);
#endif
