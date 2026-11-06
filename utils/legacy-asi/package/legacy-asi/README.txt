reVC legacy-asi directory
=========================

This directory is included in every build so the mod location is visible.
Its presence does NOT enable original ASI support in a normal x64/WGPU build.
The default enabled.ini is deliberately empty: no third-party code is enabled.

Original Vice City ASIs are Windows PE32 x86 DLLs. They require the separate
experimental x86 legacy-ABI build with reVC.exe, reVC-game.exe and reVC-core.dll.
Only the reviewed, limited game ABI is implemented; arbitrary ASIs are not
automatically compatible. Resource mods belong in modloader/, not here.

Linux: original Windows ASIs do NOT load inside the native ELF reVC process.
Use the separate experimental Linux/Wine x86 package and its ./reVC-legacy-asi
launcher (Python 3 and Wine are required). This does not add ASI support to
the native Linux executable, native WGPU, ARM, or a Windows x64 executable.

Installing an ABI-reviewed ASI in the matching legacy build:
1. Close the game and copy the ASI plus its adjacent INI/resources here.
2. Add its flat filename and exact SHA-256 in enabled.ini under [Modules]:
     ReviewedMod.asi=THE_64_HEXADECIMAL_DIGIT_SHA256_OF_THE_REVIEWED_BINARY
   Do not copy the placeholder literally. Do not use paths or duplicate names.
3. Launch the legacy game, not the native/x64 executable.

A matching hash identifies a file; it does not prove compatibility or safety.
ASIs execute with your user privileges. Do not enable an unreviewed binary
merely because you know its hash. Missing/empty [Modules] loads no ASIs.
Rebuilding/repackaging creates missing defaults but preserves enabled.ini.

See the source project's docs/LegacyASI.md and docs/ModCompatibility.md for
the supported profiles, installation details and current limitations.
