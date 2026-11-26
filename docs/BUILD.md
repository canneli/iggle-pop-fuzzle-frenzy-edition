# Código do mod

O renderizador 32 bits usa LLVM-MinGW. No Windows, execute:

```powershell
.\build-iggle.ps1 -Toolchain 'C:\caminho\para\llvm-mingw\bin'
```

A saída é `ddraw.dll`. O fonte inclui a integração com o jogo, os controles, o editor e o código do instalador e da troca de idioma. O empacotador `pack-verified.cjs` recebe o arquivo SAF da instalação local e uma pasta de recursos por idioma. Os deltas e recursos do instalador estão embutidos no executável da release.

## v34 assets and dialogue

The renderer build also compiles `src/iggle_voice.c`; its Vorbis decoder is pinned in `inc/third_party/stb_vorbis.c`. No external codec installation is needed. The decoder license is in `docs/licenses/stb_vorbis.txt`.

Compressed dialogue is in `assets/voices/{en,pt-BR}/`. Install those folders as `iggle-hd/voices/{en,pt-BR}/` beside the game's executable. Each language uses `hey.ogg`, `lost.ogg`, `wait.ogg` and `help.ogg`. The decoder accepts mono Vorbis from 16 to 48 kHz; the shipped clips are 22.05 kHz. Native cheers are not intercepted or muted by v34.

Character replacements are in `assets/character-remake/export/res/`, with targets and original dimensions recorded in `assets/character-remake/resource-map.json`. Preserve native dimensions and paired transparency masks when editing them. `pack-verified.cjs` verifies each SAF entry and directory digest before and after packing; copying bytes into an archive without updating those digests can prevent the game from loading.

The published installer bundles the complete previous mod payload with the remake and dialogue. Personal saves, registry profiles, custom stages and author-specific settings are not part of these assets. The final v34 runtime changes were left for player testing; publishing the build does not imply a full campaign verification.
