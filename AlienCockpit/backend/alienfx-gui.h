#pragma once

#include <wtypes.h>
#include <CommCtrl.h>
#include <windowsx.h>
#include "../src/resource.h"
#include "ConfigHandler.h"

#ifdef _DEBUG
#define DebugPrint(_x_) OutputDebugStringA(string(_x_).c_str());
#else
#define DebugPrint(_x_)
#endif

#define ind(_x, _y) (_y * conf->mainGrid->x + _x)

extern HINSTANCE hInst;
extern ConfigHandler* conf;
extern HWND mDlg;
extern groupset* activeMapping;
