#pragma once

#include <cstdint>

#include "uiohook.h"

struct NativeEventData {
  uint16_t type;
  uint16_t mask;
  uint64_t time;

  bool hasKeyboard;
  uint16_t keycode;
  uint16_t rawcode;
  uint16_t keychar;

  bool hasMouse;
  uint16_t button;
  uint16_t clicks;
  int16_t mouseX;
  int16_t mouseY;

  bool hasWheel;
  uint16_t amount;
  uint16_t wheelClicks;
  int16_t direction;
  int16_t rotation;
  int16_t wheelType;
  int16_t wheelX;
  int16_t wheelY;
};