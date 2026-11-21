# Direct Input Listener Library - DILL

## Overview

The purpose of DILL is to provide joystick device information, including:
* device addition and removal
* device specification
* change in device state

This information is exposed via a simple C style API such that other languages, such as Python, can easily interface with it. The desired information is obtained from the DirectInput system as well as Windows' event system.

Xbox-compatible gamepads are read through Windows XInput. DILL uses the Windows
product name and adds the XInput slot number. If Windows cannot identify the
model, the name is **XInput Gamepad** followed by the slot number. When Windows
offers multiple names, the first name gets a **+** and additional names follow
on separate lines for use in tooltips. The GUID is derived from the Windows
slot, so profile bindings follow the slot if Windows
reassigns controllers.
Rebind profiles that used a gamepad's previous DirectInput GUID. The two triggers
are independent axes (Z for left and Rz for right). Sticks use X/Y and Rx/Ry, and
the directional pad reports as a POV hat.

Regular HID joysticks and gamepads without XInput support use DirectInput.

## Usage

The main way of interacting with DILL occurs via the two callbacks exposed by the library.

The `set_input_event_callback` function allows setting the callback responsible to inform the the using code about the addition or removal of a device. The callback has the following form `void device_change_callback(DeviceSummary info, DeviceActionType action)`.

The `set_device_change_callback` is execute whenever a device changes its state and takes a callback of the following form `void event_callback(JoystickInputData data)`.
