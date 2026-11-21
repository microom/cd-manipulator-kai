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

「読めない最終セクタを0補完する」は初期値がオンです。混在CDでディスク末尾のMODE1データセクタ1つが読み取りエラーになった場合、2352バイトのゼロで補完して保存します。最終セクタは単独で読み、データ用の2方式がともに訂正不能を報告したら補完に進みます。補完した位置はログに記録します。ディスクから読めたデータだけを保存したいときは、この設定をオフにします。

「不一致の音声を自動補正」は休止中です。高度な設定ではグレー表示になります。

## ビルド

Visual Studio 2026 の C++ デスクトップ開発と x86 MFC、CMake 4.2 以降を使用します。x86 Native Tools Command Prompt で実行します。

```cmd
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`build/Release/CdManipulatorKai.exe` と同じフォルダに、元の配布ZIPに入っている `.cml` 言語ファイルを置きます。ヘルプを使う場合は `CdManipulator.chm` も置きます。

## 配布用ZIPを作る

7-ZipとPython 3.10以降を用意します。`dist` にKaiの実行ファイルと同梱用言語ファイル・ヘルプを揃えます。MAMEの公式パッケージを2つ入手します。[Windows x64バイナリ](https://github.com/mamedev/mame/releases/download/mame0287/mame0287b_x64.exe) · [対応ソース](https://github.com/mamedev/mame/releases/download/mame0287/mame0287s.exe)

```powershell
python tools/package-release.py --kai-dir dist --mame-bin mame0287b_x64.exe --mame-source mame0287s.exe --out build/release
```

生成された実行用ZIPと `MAME-0.287-source.7z` を同じGitHub Releaseに添付します。

## 出典とライセンス

Kaiの元の配布元は [物置部屋。の CD Manipulator](http://www.storeroom.info/cdm/) です。Kai本体の著作権表示、非商用条件を含む再配布条件、免責事項は [cdm-license.txt](src/cdm-license.txt) を参照してください。

同梱の `chdman.exe` は、MAME 0.287公式Windows x64版の実行ファイルです。CHD生成時に独立したプログラムとして動作し、MAMEのライセンス条件に従って配布します。著作権表示とライセンス全文はZIP内の `licenses/mame/COPYING` と `licenses/mame/docs/legal/` にあります。対応ソースは同じReleaseから取得できます。[MAMEのライセンス情報](https://docs.mamedev.org/license.html)
