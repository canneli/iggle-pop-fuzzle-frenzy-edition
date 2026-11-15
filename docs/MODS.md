# Creating Iggle Pop mods

Install v30 or later. Place `.igglemod` files in the `mods` folder inside the Steam game directory. Open **Mods** on the main menu to refresh the list. One world and one Fizzy skin can be active together. Restart the game after switching skins to refresh loaded sprites.

## Make a package

Download either example from the [v29 release](https://github.com/canneli/iggle-pop-fuzzle-frenzy-edition/releases/tag/v29). Run the [packaging tool](../tools/igglemod.py) with Python 3.

```sh
python igglemod.py extract Sugar-Grove.igglemod my-world
python igglemod.py pack world my-world "My World" my-world My-World.igglemod
```

To build a skin, extract `Berry-Fizzy.igglemod` and use `hero` in place of `world`. Keep the names of the extracted files. IDs accept lowercase letters, numbers and hyphens. Display names accept up to 30 printable ASCII characters. JPG sprites and their underscore-prefixed grayscale PNG masks share the same dimensions.

A world includes a sample `stage.xml`. To edit it in-game, copy it into `CustomLevels` under a new filename, enable the world and open the copy in the level editor. The packer updates the sample stage's theme identifier automatically. Editor previews use `.rgba` files with little-endian 32-bit width and height followed by BGRA pixels.

## Português (Brasil)

Instale o patch. Na pasta do jogo, coloque arquivos .igglemod na pasta mods.
No menu inicial, abra Mods (atalho M), selecione um pacote para ativá-lo
ou desativá-lo e use Jogar mundo. A lista é atualizada ao abrir o menu.

Você pode ter um mundo e um visual de Fizzy ativos ao mesmo tempo. O
mundo ativo também aparece no seletor de temas do editor. Reinicie o
jogo depois de mudar de visual para limpar as imagens já carregadas.

Para criar um pacote, use Python 3, sem bibliotecas extras:

  python igglemod.py extract Sugar-Grove.igglemod meu-mundo
  python igglemod.py pack world meu-mundo "Meu Mundo" meu-mundo Meu-Mundo.igglemod

Para criar um visual de Fizzy, extraia Berry-Fizzy.igglemod e use hero
no lugar de world. O identificador aceita letras minúsculas, números e
hífens. Use um nome de exibição de até 30 caracteres ASCII.

Use os arquivos extraídos como modelos. Cada par JPG/PNG tem o mesmo
tamanho. O PNG com prefixo _ é a máscara de transparência em tons de
cinza. As prévias .rgba do mundo começam com largura e altura uint32
little-endian seguidas por pixels BGRA. stage.xml é a fase de exemplo.
O empacotador atualiza editorTheme ao trocar o ID. Para editar a fase
de exemplo no jogo, copie o stage.xml extraído para CustomLevels com
um nome próprio, ative o mundo e abra a cópia pelo editor. Os pacotes de herói usam as quatro direções do
Fizzy e mantêm seus atributos e animações de jogo.
