# CD Manipulator Kai

Y.Kanechika（ごーるど）さんの CD Manipulator 2.70 Final をベースにした、Windows 向けの CD イメージ作成ツールです。Kai版は microom がメンテナンスしています。

- シングルセッションの自動選択で、音声とデータが混在する CD に対応。
- 本編のセクタをまとめて読み取り、確認済みの音声終端を越える追加読み取りを省略。
- `chdman.exe` があれば、吸い出した IMG・CUE から CHD も生成可能。
- 読み取り中の進捗、トラック番号、経過時間を表示。

各バージョンの変更は [更新履歴](CHANGELOG.md) を参照してください。

## セットアップ

1. [CD Manipulator Kaiの最新版](https://github.com/microom/cd-manipulator-kai/releases/latest)からWindows版のZIPをダウンロードして展開し、`CdManipulator.exe` を起動します。 初回に日本語で使う場合は `Japanese.cml` を選びます。
2. CHDも作る場合は、[mame0287b_x64.exe（MAME公式・Windows x64）](https://github.com/mamedev/mame/releases/download/mame0287/mame0287b_x64.exe) をダウンロードして実行し、展開先を指定します。展開先の `chdman.exe` を `CdManipulator.exe` と同じフォルダにコピーします。

## 使い方

### CHDも生成する

「CDイメージ作成」→「高度な設定」で「CHDも生成する」をオンにすると、指定したCUEと同じ場所にCHDができます。

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

`build/Release/CdManipulator.exe` と同じフォルダに、元の配布ZIPに入っている `.cml` 言語ファイルを置きます。ヘルプを使う場合は `CdManipulator.chm` も置きます。

## 出典とライセンス

元の配布元は [物置部屋。の CD Manipulator](http://www.storeroom.info/cdm/) です。元の著作権表示、再配布条件、免責事項は [cdm-license.txt](src/cdm-license.txt) を参照してください。改変版の配布にも同じ条件を適用します。商業利用は禁止されています。

CHD生成には独立した外部プログラムを使います。[MAME公式配布](https://github.com/mamedev/mame/releases)と [MAMEのライセンス情報](https://docs.mamedev.org/license.html)を参照してください。
