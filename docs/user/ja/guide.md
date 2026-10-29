# Parallaxa ユーザーガイド

Parallaxa は 2D VTuber 向けの無料の OBS Studio プラグイン（GPL）です。シーンのレイヤーに奥行きと視差を付け、仮想カメラの動き（ズーム、パン、回転、揺れ、自動ドリフト、音声に反応するパンチ）、カラールック、エフェクト、レイヤーごとの被写界深度ぼかしを提供します。単体で動き、どのアバターソース（Spout2 経由の VTube Studio、PNGTuber の画像、Web カメラ）でも使えます。AI Live Studio などのアプリは obs-websocket（`docs/protocol/vendor-api.md`）を通じてリモート操作できます。

English: [../en/guide.md](../en/guide.md) · Bahasa Indonesia: [../id/guide.md](../id/guide.md)

## 1. 必要なもの

- Windows 10（1803 以降）または 11、64 ビット
- OBS Studio 32.0 以降（Steam 版または obsproject.com のインストーラー版）

## 2. インストール

1. **OBS を終了します。**
2. `Parallaxa-<バージョン>-Setup.exe` を実行し、管理者の確認を許可します。
3. **OBS Studio** のページでフォルダーを確認します。セットアップは公式インストーラー版、Steam のライブラリ、`C:\Program Files\obs-studio` を探します。別の OBS を使っている場合は **参照** から `bin\64bit\obs64.exe` を含むフォルダーを選んでください。
4. セットアップは OBS のバージョンを読み取り、プラグインの配置を選びます。
   - **OBS 33 以降:** `C:\ProgramData\obs-studio\plugins\parallaxa\parallaxa.dll`
   - **OBS 32:** `C:\ProgramData\obs-studio\plugins\parallaxa\bin\64bit\parallaxa.dll`
   - **OBS が見つからない場合:** 両方（OBS 33 以降は 1 つ目だけを読み込み、2 つ目は無視します）

   OBS 32 では **OBS 33 以降用のコピーもインストールする** に最初からチェックが付いていて、両方のコピーがインストールされます。Steam は OBS を自動で更新するため、33 に更新された後もプラグインが読み込まれます。
5. OBS を起動します。確認するには **ヘルプ > ログファイル > 現在のログを表示** を開き、`[parallaxa] loaded` を探します。

**手動インストール（zip）:** `Parallaxa-<バージョン>-windows-x64.zip` を `C:\ProgramData\obs-studio\plugins\` に展開し、`...\plugins\parallaxa\` ができるようにします。zip には両方の配置が入っているので、OBS 32 でも 33 でも動きます。

## 3. 10 秒で最初のシーン

OBS で **ツール > Parallaxa: 現在のシーンをセットアップ** を選びます。現在のシーンに **Parallaxa Camera Rig** と **Parallaxa FX** フィルターが追加されます。ソース名から役割を推測します: `bg` / `background` / `背景` は背景、`vts` / `spout` / `avatar` / `model` はキャラクター、`fg` / `foreground` / `前景` は前景です。そして Cozy Chat のルックを適用します。各レイヤーの奥行きはフィルターのプロパティで調整します。被写界深度を使うには、背景や前景のソースに **Parallaxa Depth Blur** を追加します。

できあがったシーンを試すには **ツール > Parallaxa: サンプルシーンをインポート** を選びます。シーン **Parallaxa Sample - Just Chatting**（オリジナルの背景・前景画像、VTube Studio の Spout2 ソース、Parallaxa のフィルター）が現在のシーンコレクションに追加されます。画像はプラグインのデータフォルダーから読み込むので、OBS がファイルの場所を尋ねることはありません。同じ名前のシーンやソースがすでにある場合は「Background 2」のように番号が付き、既存のものは上書きされません。同じコレクションは `data\presets\samples\parallaxa_sample.json` にもあり、**シーンコレクション > インポート** で読み込めます。

## 4. Parallaxa ドック

フィルターのプロパティを開かずに、カメラ、ルック、エフェクト、レイヤーを操作できます。**ドック > カスタムブラウザドック** で次の URL を追加します。

```
file:///C:/ProgramData/obs-studio/plugins/parallaxa/data/dock/dock-standalone.html
```

obs-websocket で OBS と直接通信します。先に **ツール > WebSocket サーバー設定** でサーバーを有効にしてください（ポート 4455）。ドックは最初に一度だけパスワードを尋ねます。

## 5. Spout2 で VTube Studio を取り込む

1. Off World Live の **OBS Spout2 プラグイン**（https://github.com/Off-World-Live/obs-spout2-plugin/releases）を、OBS のバージョンに合わせてインストールします（OBS 32: **1.13.0**、OBS 33 以降: **1.14.0**）。
2. VTube Studio の設定で **Spout2 出力** をオンにします。
3. OBS で **Spout2 Capture** ソースを追加し、VTube Studio を選びます。名前を `VTS` や `Avatar` にしておくと、**Parallaxa: 現在のシーンをセットアップ** がキャラクターとして認識します。
4. **VTube Studio と OBS を同じ GPU で動かします。** 内蔵グラフィックスと NVIDIA / AMD のカードの両方がある PC では、Spout2 ソースが黒いままになることがあります。Windows の **設定 > システム > ディスプレイ > グラフィック** で `VTube Studio.exe` と `obs64.exe` を **高パフォーマンス** に設定し、両方を再起動します。

## 6. トラブルシューティング

| 症状 | 対処 |
|---|---|
| OBS に Parallaxa のフィルターがない | セットアップ中に OBS を終了していたか確認します。OBS のログで `parallaxa` を検索します。`MODULE_INCOMPATIBLE_VER` は OBS が 32.0 より古いという意味です。チェックを外したまま OBS が 32 から 33 に更新された場合は、インストーラーをもう一度実行します。 |
| セットアップに「OBS Studio が起動しています」と出る | トレイアイコンも含めて OBS を終了し、**再試行** をクリックします。 |
| ドックが接続できない | OBS の WebSocket サーバーを有効にし、ポート 4455 を確認して、ドックでパスワードを入力し直します。 |
| Spout2 ソースが黒いまま | VTube Studio と OBS を同じ GPU に設定します（5 章の手順 4）。OBS のバージョンに合った Spout2 プラグインか確認します。 |

## 7. アンインストール

OBS を終了してから **設定 > アプリ > インストールされているアプリ** で「Parallaxa (OBS plugin)」をアンインストールします。念のため、先に OBS で **ツール > Parallaxa: すべての基本位置に戻す** を選び、シーンのアイテムを保存された位置に戻しておきます。

## 8. ライセンス

Parallaxa は GNU GPL v2 以降のフリーソフトウェアです（プラグインフォルダーの `LICENSE.txt`）。ソースコードの場所は `SOURCE.txt` に書かれています。各リリースではバイナリと一緒に `Parallaxa-<バージョン>-source.zip` を公開します。サードパーティのコンポーネントは `THIRD_PARTY_NOTICES.md` に記載しています。
