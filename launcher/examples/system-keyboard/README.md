# System keyboard import

The public payload SDK supplies the IME dialog imports but omits
`sceCommonDialogInitialize`. This link-only facade follows ProsperoRadio's
native integration. It contains no system binary or implementation.

Run `bash examples/system-keyboard/build-import.sh` before the app build and
append `build/system-keyboard/libSceCommonDialog.so` to `APP_IMPORT_STUBS`.
The native builder resolves the symbol to `libSceCommonDialog.sprx` on the
console. Do not add the facade to runtime modules or package it with the app.

The UI foundation's `platform/ps5/ime.hpp` provides bounded UTF-8/UTF-16 text
entry and dialog lifecycle handling. The facade alone does not open a dialog.
