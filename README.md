# maze3d
Pythonによるmap editor、C++による描画

## ゲームの操作

| キー | 動作 |
|---|---|
| ↑ / W | 1マス前進 |
| ← / A、→ / D | 左右に90°ターン |
| ↓ / S | 180°ターン |
| E | 階段を使う |
| Enter / Space / Z | メッセージを送る・決定 |
| ↑↓ | はい／いいえ の選択 |
| X / Backspace | いいえ |
| Q / Esc | 終了 |

起動時は `map_editor/utsuro.json` を読み込みます。別のマップは引数で指定できます（例: `maze3d.exe map_editor/mymap00.json`）。

## キャラクター

`assets/data/characters.json` に主人公と仲間候補を定義します（`hero` が主人公のID）。

| 項目 | 内容 |
|---|---|
| `hp` | 最大HP |
| `str` | ちから（攻撃力） |
| `vit` | たいりょく（防御・最大HP） |
| `agi` | すばやさ（行動順・回避・逃走） |
| `gold` | 所持金（各人が持つ。宝箱のゴールドはリーダーが受け取る） |
| `portrait` | `assets/portraits/` の顔グラフィック（64×64 PNG。無ければ仮のシルエット） |

パーティは最大5人で、画面右の欄に表示されます。マップの「人間（勧誘）」イベントで仲間に誘えます。

## 依存ライブラリ（vcpkg, x64-windows）

`sdl2`、`sdl2-image`、`sdl2-ttf`、`nlohmann-json`

```
vcpkg install sdl2:x64-windows sdl2-image:x64-windows sdl2-ttf:x64-windows nlohmann-json:x64-windows
```
