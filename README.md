# maze3d
Pythonによるmap editor、C++による描画

## ゲームの操作

| キー | 動作 |
|---|---|
| ↑ / W | 1マス前進 |
| ← / A、→ / D | 左右に90°ターン |
| ↓ / S | 180°ターン |
| E | 階段を使う |
| Enter / Space / Z | メッセージを送る |
| Q / Esc | 終了 |

起動時は `map_editor/utsuro.json` を読み込みます。別のマップは引数で指定できます（例: `maze3d.exe map_editor/mymap00.json`）。

## 依存ライブラリ（vcpkg, x64-windows）

`sdl2`、`sdl2-image`、`sdl2-ttf`、`nlohmann-json`

```
vcpkg install sdl2:x64-windows sdl2-image:x64-windows sdl2-ttf:x64-windows nlohmann-json:x64-windows
```
