param(
    [string]$Out = "loko.exe",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($Clean) {
    Remove-Item -ErrorAction SilentlyContinue *.obj,*.pdb,*.ilk,$Out
    exit 0
}

if (-not (Test-Path "imgui")) {
    Write-Error "imgui/ not found. Drop ImGui 1.89+ sources (core + imgui_impl_win32 + imgui_impl_dx11) into ./imgui/ beside ./src/."
    exit 1
}

$src = @(
    "src/main.cpp",
    "src/game.cpp",
    "src/overlay.cpp",
    "src/esp.cpp",
    "src/menu.cpp",
    "src/aimbot.cpp",
    "src/console.cpp",
    "src/mem.cpp",
    "src/portrait.cpp",
    "src/portrait_images.cpp"
)

$imgui = @(
    "imgui/imgui.cpp",
    "imgui/imgui_draw.cpp",
    "imgui/imgui_tables.cpp",
    "imgui/imgui_widgets.cpp",
    "imgui/imgui_impl_win32.cpp",
    "imgui/imgui_impl_dx11.cpp"
)

$files = $src + $imgui

$flags = @(
    "/std:c++17",
    "/EHsc",
    "/O2",
    "/MD",
    "/W4",
    "/wd4100",
    "/wd4505",
    "/wd4201",
    "/wd4324",
    "/wd4505",
    "/wd4267",
    "/DWIN32",
    "/D_WINDOWS",
    "/DUNICODE",
    "/D_UNICODE",
    "/I.",
    "/Iimgui",
    "/Isrc",
    "/Fe:$Out"
)

$link = @("/link", "/SUBSYSTEM:WINDOWS", "/MACHINE:X64")

& cl @flags @files @link
exit $LASTEXITCODE
