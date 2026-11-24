# CD Manipulator Kai

Y.Kanechika（ごーるど）さんの CD Manipulator 2.70 Final をベースにした、Windows 向けの CD イメージ作成ツールです。Kai版は microom がメンテナンスしています。

- シングルセッションの自動選択で、音声とデータが混在する CD に対応。
- 本編のセクタをまとめて読み取り、確認済みの音声終端を越える追加読み取りを省略。
- 同梱の `chdman.exe` で、吸い出したIMG・CUEからCHDも生成可能。
- 読み取り中の進捗、トラック番号、経過時間を表示。

各バージョンの変更は [更新履歴](CHANGELOG.md) を参照してください。

## セットアップ

1. [CD Manipulator KaiのRelease](https://github.com/microom/cd-manipulator-kai/releases/latest)からWindows x64版のZIPをダウンロードして展開します。
2. `CdManipulatorKai.exe` を起動します。初回に日本語で使う場合は `Japanese.cml` を選びます。

## 使い方

### CDを吸い出してCHDも生成する

1. ディスクを入れ、「CDイメージ作成」でドライブと出力先のCUEを選びます。読み取り方式は通常「自動（シングルセッション）」を使います。
2. CHDも作る場合は「高度な設定」で「CHDも生成する」をオンにして開始します。CHDは指定したCUEと同じ場所に保存されます。

配布ZIPには `chdman.exe` が入っています。

「CHD作成後にIMG/CUEを削除」は初期値がオフです。オンにすると、CHDの整合性検査と保存が成功したあとに、変換元のIMGとCUEを削除します。

### 混在CDの最終セクタ

「読めない最終セクタを0補完する」は初期値がオンです。混在CDでディスク末尾のMODE1データセクタ1つが読み取りエラーになった場合、2352バイトのゼロで補完して保存します。最終セクタは単独で読み、データ用の2方式がともに訂正不能を報告したら補完に進みます。補完した位置はログに記録します。この設定をオフにすると、ディスク末尾での0補完を無効にできます。

### PCE-CDの音声プリギャップ

自動読み取りでは、MODE1データから音声へ切り替わる150セクタのプリギャップを対象に、先頭1セクタだけが訂正不能だった時に限って0補完します。直前のデータセクタと後続の音声位置を確認し、後続149セクタを全部読み取れた場合に、補完した1セクタとともにIMGに保存します。CUEには保存済みの `INDEX 00` を記録します。補完した位置はログに、補完したことは完了画面に出ます。

「不一致の音声を自動補正」は休止中です。高度な設定ではグレー表示になります。

## ビルド

Visual Studio 2026 の C++ デスクトップ開発と x86 MFC、CMake 4.2 以降を使用します。x86 Native Tools Command Prompt で実行します。

```cmd
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

ビルド時に `languages/` の3種類の `.cml` がEXEと同じフォルダーにコピーされます。Kaiで追加した表示は `#Kai.` で始まる項目を編集できます。追加項目のない旧辞書を選んだ場合、その項目は内蔵の英語表記になります。ヘルプを使う場合は `CdManipulator.chm` も置きます。

## 配布用ZIPを作る

7-ZipとPython 3.10以降を用意し、`dist` にKaiの実行ファイルとヘルプを揃えます。言語ファイルは `languages/` から同梱します。[MAME 0.287公式Windows x64バイナリ](https://github.com/mamedev/mame/releases/download/mame0287/mame0287b_x64.exe) を入手します。

```powershell
python tools/package-release.py --kai-dir dist --mame-bin mame0287b_x64.exe --out build/release
```

実行用ZIPをGitHub Releaseに添付し、公開済みの対応ソースへの直リンクをリリース説明にも載せます。対応ソースのアーカイブを作るときは、[MAMEの公式ソースパッケージ](https://github.com/mamedev/mame/releases/download/mame0287/mame0287s.exe) を入手し、`--mame-source mame0287s.exe` を指定します。

## 出典とライセンス

Kaiの元の配布元は [物置部屋。の CD Manipulator](http://www.storeroom.info/cdm/) です。Kai本体の著作権表示、非商用条件を含む再配布条件、免責事項は [cdm-license.txt](src/cdm-license.txt) を参照してください。

同梱の `chdman.exe` は、MAME 0.287公式Windows x64版の実行ファイルです。CHD生成時に独立したプログラムとして動作し、MAMEのライセンス条件に従って配布します。著作権表示とライセンス全文はZIP内の `licenses/mame/COPYING` と `licenses/mame/docs/legal/` にあります。[MAMEのライセンス情報](https://docs.mamedev.org/license.html)

対応ソースはKaiの `v2.70-kai.8` で公開しています。[MAME 0.287 対応ソース（.7z）](https://github.com/microom/cd-manipulator-kai/releases/download/v2.70-kai.8/MAME-0.287-source.7z)
