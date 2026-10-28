# maze3d Map Editor

Tkinterで作成したmaze3d用の2Dマップエディタです。maze3d本体とは別アプリで動き、JSONファイルを入出力します。外部Pythonパッケージは使いません。

## 起動

WindowsにPython 3.10以降をインストールし、`tkinter` を含む標準構成を選んでください。プロジェクト専用の仮想環境は次のように作成できます。

```powershell
cd C:\projects\maze3d\map_editor
python --version
python -m venv .venv
Test-Path .\.venv\Scripts\Activate.ps1
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\.venv\Scripts\Activate.ps1
python editor.py
```

`Test-Path` の結果が `True` になってから仮想環境を有効にしてください。`False` の場合、仮想環境の作成に失敗しています。`python --version` がエラーになる場合やMicrosoft Storeが開く場合は、Python 3.10以降をインストールしてからPowerShellを開き直してください。

PowerShellで `Activate.ps1` を実行するには実行ポリシーの許可が必要です。上記の `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` は**現在開いているPowerShellだけ**でスクリプト実行を許可するため、管理者権限や恒久的なポリシー変更は不要です。確認が表示された場合は `Y` を入力してください。

仮想環境を有効化せずに起動する場合は、次を実行してください。

```powershell
.\.venv\Scripts\python.exe editor.py
```

追加のpipパッケージはありません。

### トラブルシューティング

`Activate.ps1` が「コマンドとして認識されません」と表示される場合、通常は `.venv` がまだ作成されていないか、作成に失敗しています。まず `python -m venv .venv` を実行し、その後 `Test-Path .\.venv\Scripts\Activate.ps1` が `True` になることを確認してください。`python` 自体が見つからない場合はPythonをインストールしてください。スクリプト実行ポリシーのエラーが出た場合は `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` を実行してから、もう一度有効化してください。または、仮想環境のPythonを直接実行すれば有効化は不要です。

## 操作

- フロア欄で編集する階を切り替えます。フロア追加時に幅・高さを指定する必要はありません。新規フロアは1×1セルで始まり、配置に応じて自動拡張されます。
- 壁またはドアのツールを選び、グリッド境界をドラッグします。ドラッグ方向に応じて横線／縦線が自動で決まり、連続する線をまとめて引けます。クリックだけの場合は近い境界に1線分を置きます。
- 点線で示す現在の範囲の外側にも、表示されている薄いグリッドの範囲内で配置できます。配置するとフロアサイズが自動で広がり、上部に現在の幅×高さが表示されます。
- 「サイズ変更...」から後で範囲を縮小できます。範囲外になる線分や階段は確認後に削除されます。
- 消去ツールで境界線や階段を消します。
- 階段ツールで1つ目のセルを選び、フロアを切り替えて2つ目のセルを選ぶと、双方向の階段を作成します。
- 階段セルには `St:1` のような階段IDと、登り側の `▲`／降り側の `▼` アイコンを表示します。
- マウスホイールを上に回すと上の階層へ、下に回すと下の階層へ切り替えます。階層順はフロア名の `B1F`、`1F`、`2F` などから判定します。
- ゲーム開始地点ツールでセルをクリックすると開始地点（青いP）を設定・移動できます。同じセルを再クリックすると開始時の向きが時計回り（北→東→南→西）に変わり、Pの外側の三角で表示されます。消去ツールで解除できます。
- イベントツールでセルをクリックすると編集ダイアログが開きます。種類は「メッセージ」（踏むと文章を表示。「1回だけ表示する」も可）、「宝箱」（ゴールド・アイテムを1回だけ入手）、「人間（勧誘）」（人物を仲間に誘える。人物は `assets/data/characters.json` から選択）、「ワープ」（踏むと指定したフロア・座標へ飛ぶ。到着時の向きも指定可）。ワープ床は紫の渦と行き先（例: `→B1F 3,4`）で表示します。既存のイベントをクリックすると編集・削除でき、消去ツールでも削除できます。
- 看板ツールでは、セルの中の「見る方向の辺」の近くをクリックして文字を入力します（例: 扉の手前のセルで扉側の辺の近く）。同じ場所をクリックすると編集でき、文字を空にすると削除します。消去ツールでセルの中央付近をクリックすると、そのセルの看板をまとめて削除します。
- 「ファイル」メニューからJSONを開く／保存します。Ctrl+O、Ctrl+Sにも対応します。
- JSONに重複する境界線があれば読み込み時に警告し、後方の定義を優先します。

## JSON仕様 v1

- `version` は現在 `1`。
- 1ファイルに `floors` 配列で全フロアを格納します。保存時の幅・高さはエディタの現在の範囲です。
- 座標の原点は左上。xは右向き、yは下向きに増えます。
- 壁・ドアの `locate` は線分の開始グリッド交点 `[x, y]`。
- `axis: "h"` は右向き、`axis: "v"` は下向きです。
- `length` は連続する線分数（グリッド単位）。省略時は1です。
- 階段の `ends` は2地点で、通常は双方向に通行できます。階段の `locate` はセル番号で、左上セルが `[0, 0]` です。
- 任意の `spawn` はゲーム開始地点で、`floorId` とセル座標 `locate`、開始時の向き `facing`（`"N"`／`"E"`／`"S"`／`"W"`、省略時は `"N"`）を指定します。省略時は開始地点未設定です。
- 任意の `events` はフロアごとのイベントマス配列です。`locate` はセル座標、`type` は `"message"`、`"treasure"`、`"recruit"` のいずれか。
  - `message`: `text`（改行は `\n`）、`once`（`true` なら1回だけ表示、省略時 `false`）。
  - `treasure`: `gold`（0以上の整数）、`item`（アイテム名、空なら無し）、`text`（開ける前に表示する任意の文章）。宝箱は1回だけ開けられます。
  - `recruit`: `character`（`assets/data/characters.json` のキャラクターID）、`text`（出会ったときのセリフ。空なら既定のセリフ）。仲間になるとその人物はマップから消えます。
  - `warp`: `to`（行き先。`floorId`、セル座標 `locate`、任意で到着時の向き `facing`。省略時は向きそのまま）、`text`（ワープ前に表示する文章。空なら無言でワープ）。到着したマスのイベントは起こりません（ワープ床どうしの往復ループを防ぐため）。
- 任意の `signs` はフロアごとの看板配列です。`locate` のセルで `facing`（`"N"`／`"E"`／`"S"`／`"W"`）を向いて立ち止まっている間だけ、画面上部に `text` を表示します。操作は止めないので、そのまま前進できます（例: 銀行の扉の前で `BANK`）。1セルに向きごとに1つまで置けます。

例:

```json
{
  "version": 1,
  "floors": [
    {
      "id": "1F",
      "width": 8,
      "height": 6,
      "edges": [
        { "axis": "h", "locate": [0, 0], "type": "wall", "length": 4 },
        { "axis": "v", "locate": [2, 1], "type": "door" }
      ],
      "events": [
        { "locate": [1, 2], "type": "message", "text": "古びた石碑がある。", "once": false },
        { "locate": [3, 4], "type": "treasure", "text": "", "gold": 120, "item": "ブロンズの鍵" },
        { "locate": [5, 2], "type": "warp", "text": "", "to": { "floorId": "B1F", "locate": [3, 4], "facing": "N" } }
      ],
      "signs": [
        { "locate": [1, 1], "facing": "E", "text": "BANK" }
      ]
    },
    { "id": "B1F", "width": 8, "height": 6, "edges": [] }
  ],
  "stairs": [
    {
      "id": "stair_1",
      "ends": [
        { "floorId": "1F", "locate": [0, 0] },
        { "floorId": "B1F", "locate": [3, 4] }
      ]
    }
  ],
  "spawn": { "floorId": "1F", "locate": [1, 1], "facing": "E" }
}
```

## Gitでの制作管理

リポジトリのルートに `.gitignore` を置き、CMakeの `build/`、Pythonの `.venv/`、キャッシュ類を除外しています。マップJSONとソースはGitに追加できます。

リポジトリ: https://github.com/namfon-com/maze3d
