param([Parameter(Mandatory=$true)][string]$Toolchain,[switch]$CustomSmoke)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
& (Join-Path $Toolchain 'i686-w64-mingw32-windres.exe') -I $root -i "$root\res.rc" -o "$root\res.o"
if($LASTEXITCODE){throw 'Resource compilation failed'}
$debugFlag = @()
if($CustomSmoke) { $debugFlag = @("-DIGGLE_CUSTOM_TEST") }
$sources=Get-ChildItem "$root\src" -Recurse -Filter '*.c' | Sort-Object FullName | ForEach-Object FullName
& (Join-Path $Toolchain 'i686-w64-mingw32-clang.exe') @debugFlag -shared -O2 -std=c99 -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-pointer-sign -I "$root\inc" -include "$root\inc\iggle_display_guard.h" @sources "$root\res.o" "$root\exports.def" -lgdi32 -lwinmm -lole32 -loleaut32 -lmsimg32 -lshell32 -lavifil32 -luuid -static '-Wl,--enable-stdcall-fixup' -o "$root\ddraw.dll"
if($LASTEXITCODE){throw 'Renderer compilation failed'}

