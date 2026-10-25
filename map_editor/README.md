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
- ゲーム開始地点ツールでセルをクリックすると開始地点（青いP）を設定・移動できます。消去ツールで解除できます。
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
- 任意の `spawn` はゲーム開始地点で、`floorId` とセル座標 `locate` を指定します。省略時は開始地点未設定です。

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
  "spawn": { "floorId": "1F", "locate": [1, 1] }
}
```

## Gitでの制作管理

リポジトリのルートに `.gitignore` を置き、CMakeの `build/`、Pythonの `.venv/`、キャッシュ類を除外しています。マップJSONとソースはGitに追加できます。

この環境ではGitコマンドが見つからなかったため、リポジトリの初期化やコミットはまだ行っていません。Git for Windowsをインストールし、VS Codeを再起動すると、ソース管理ビューから初期化・コミットできます。Gitコマンドを使える環境なら、プロジェクトルートで `git init` を実行して開始してください。
