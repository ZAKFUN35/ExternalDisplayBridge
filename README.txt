External Display Bridge
=======================
Use any laptop or PC as a second monitor via USB capture card.
Minimum input lag, DirectX 11 rendering, 1080p @ 60 FPS.


REQUIREMENTS
------------
- Windows 10/11 (64-bit)
- USB 3.0 capture card (YUY2 mode recommended)
- No additional software required -- just unzip and run!


HOW TO USE
----------
1. Connect capture card to USB 3.0 port
2. Connect source device (laptop/PC) via HDMI to capture card
3. Double-click capture_bridge.exe
4. On first launch, the program will prompt you to configure your control keys.
5. Select your capture device from the list
6. Enjoy!


CONTROLS
--------
In version 3.0, controls are fully customizable. You can bind any keyboard key or mouse button (including side buttons X1/X2).

Default settings:
F     - Toggle FPS overlay (shows FPS, codec, VSync status)
V     - Toggle VSync on/off
ESC   - Exit

Note: Your preferences are saved in keybindings.bin. To reset your controls, simply delete this file and restart the app.


VSYNC GUIDE
-----------
VSync OFF  - Lowest possible input lag (~1-3ms software delay)
             May cause screen tearing in fast-moving scenes
VSync ON   - No tearing, locked to monitor refresh rate (60 FPS)
             Adds ~16ms of display latency


FOLDER STRUCTURE
----------------
ExternalDisplayBridge/
  capture_bridge.exe       <- Main application (prebuilt)
  keybindings.bin          <- Your control settings file (automatically created)
  opencv_world4120.dll     <- Required library
  capture_bridge.cpp       <- Source code (for developers)
  README.txt               <- This file


TECHNICAL DETAILS
-----------------
- DirectX 11 rendering with FLIP_DISCARD + ALLOW_TEARING
- Triple buffering with atomic swap (zero-copy between threads)
- GPU color swizzling (BGR->RGB in HLSL shader, no CPU conversion)
- MMCSS "Pro Audio" / "Games" thread priority
- REALTIME_PRIORITY_CLASS process priority
- Auto-detects capture card resolution (720p to 1080p)


TESTED ON
---------
- Intel Core i3-10110U + Intel UHD Graphics
- USB 3.0 capture card (1920x1080 @ 60 FPS, YUY2)
- Windows 11


WANT TO IMPROVE THE CODE?
--------------------------
Know how to improve — improve, know how to optimize — optimize!

Author: ZAKFUN35
Signature: T_T
