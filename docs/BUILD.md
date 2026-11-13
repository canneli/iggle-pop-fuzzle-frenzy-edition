# Código do mod

O renderizador 32 bits usa LLVM-MinGW. No Windows, execute:

```powershell
.\build-iggle.ps1 -Toolchain 'C:\caminho\para\llvm-mingw\bin'
```

A saída é `ddraw.dll`. O fonte inclui a integração com o jogo, os controles, o editor e o código do instalador e da troca de idioma. O empacotador `pack-verified.cjs` recebe o arquivo SAF da instalação local e uma pasta de recursos por idioma. Os deltas e recursos do instalador estão embutidos no executável da release.
