-- Build only the native reVC bridge and its supported resource plugins.
-- Run in the pinned Mod Loader source directory; build.ps1 handles this.
workspace "modloader-revc"
    location "build-revc"
    configurations { "Release" }
    platforms { "x64", "x86" }
    language "C++"
    cppdialect "C++14"
    systemversion "latest"
    staticruntime "On"
    characterset "MBCS"
    symbols "On"
    buildoptions { "/bigobj", "/Zc:threadSafeInit-" }
    defines { "INJECTOR_GVM_HAS_TRANSLATOR", 'INJECTOR_GVM_PLUGIN_NAME="Mod Loader Plugin"', "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "_SCL_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS" }
    includedirs { "include", "deps/cereal/include", "deps/injector/include", "deps/boost", "deps/tinympl", "deps/utf8-cpp/source", "src/shared" }
    targetdir "bin-revc/%{cfg.platform}/%{cfg.buildcfg}"
    objdir "obj-revc/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"
    filter "platforms:x64"
        architecture "x86_64"
    filter "platforms:x86"
        architecture "x86"
    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"
    filter {}

project "addr"
    kind "StaticLib"
    files { "src/translator/**.cpp" }

project "modloader"
    kind "SharedLib"
    targetextension ".asi"
    files { "src/core/**.cpp" }
    includedirs { "src/core" }
    forceincludes { "stdinc.hpp" }
    links { "addr", "shlwapi", "dbghelp" }

for _, name in ipairs { "std.fx", "std.stream", "std.data", "std.text", "std.scm", "std.sprites", "std.movies" } do
    project(name)
        kind "SharedLib"
        files { "src/plugins/gta3/" .. name .. "/**.cpp", "src/plugins/gta3/" .. name .. "/**.cc" }
        includedirs { "src/shared/game/gta3" }
        if os.isfile("src/plugins/gta3/" .. name .. "/stdinc.hpp") then
            includedirs { "src/plugins/gta3/" .. name }
        else
            includedirs { "src/shared/stdinc/gta3" }
        end
        forceincludes { "stdinc.hpp" }
        links { "addr" }
end
