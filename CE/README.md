# PopSNES — SHARP Brain (Windows CE) 移植版

**PopSNES は非商用に限ります。** 販売すること、商用の製品や活動に使うこと、お金を払った人だけに配ることはできません(Snes9x のライセンスによる。詳しくは [`LICENSING.md`](LICENSING.md))。

## 概要

- スーパーファミコンのエミュレータ [snes9x2002](https://github.com/libretro/snes9x2002) の libretro 版を、SHARP の電子辞書 **Brain PW-G5300**(Windows CE / ARM)向けに移植したものです
- `libretro/libretro.c` をそのままコンパイルし、Win32 のフロントエンド(`CE/` 以下)を新しく書いて繋いでいます。コア側で変えたところは [`LICENSING.md`](LICENSING.md) の2節にまとめています
- 市販・自作を問わず `.smc` / `.sfc` / `.fig` / `.swc` などの SNES ROM を実行できます。この端末の処理性能の関係で、フレームスキップを使っても実機同等の速度で快適に遊べるとは限りません(負荷の軽い ROM ほど良好です)
- 特殊チップは、S-DD1 / SA-1 / SuperFX / DSP-1 / DSP-2 / C4 / S-RTC に対応しています。このうち実機で動作を確かめたのは、SuperFX(『スターフォックス』。非常に低速)と S-DD1(『ストリートファイター ZERO 2』。低速)です。SA-1 / DSP-1 / DSP-2 / C4 / S-RTC の動作は確かめていません。
- 次の特殊チップや周辺機器を使うゲームには対応していません：SPC7110(『天外魔境ZERO』など)、OBC1、DSP-3、DSP-4、Seta の ST010・ST011・ST018、BS-X(サテラビュー)、スーパーゲームボーイ、スーファミターボ。
- ゲームの ROM は同梱していません。利用者が合法的に用意したものを使ってください
- SHARP・任天堂とは関係のない、非公式のファンプロジェクトです

### 主な機能

- 日本語 / 英語の UI(Galmuri14 と東雲 16 ドットのビットマップフォントを `AppMain.exe` に内蔵)
- 画面の表示倍率(x1 / x1.5 / Wide / Full)、GDI で直接描画
- 透過エフェクトの ON/OFF
- フレームスキップ(0〜30、処理落ち時の自動発動あり)
- サウンド出力(レート・バッファサイズ・ビット深度・リサンプル品質)
- 入力キーのリマップ(斜め入力を含む)
- ステートセーブ / ロード(セーブ前に確認)、`.srm` カートリッジ SRAM の自動セーブ
- 画面の保存(スクリーンショット。`AppMain.exe` と同じフォルダの `Screenshots` に BMP で保存)
- 日本語のファイル名・フォルダ名に対応した ROM 選択画面(前回開いたフォルダから始まります)
- デバッグログ出力のトグル(既定は OFF)

## ダウンロード

ビルド済みの実行ファイルを [Release](../../../releases) に置いています。

## ビルド方法

- ツール
  * cegcc(`arm-mingw32ce-*`、`/opt/cegcc`)— Windows CE / ARM 向けのクロスコンパイラ。製作者は WSL(Windows 上の Linux)でビルドしています
- サブモジュールは使っていません
- 手順
  * `CE/` で `make && make strip` を実行します
  * `CE/AppMain.exe` ができます(依存する DLL は `COREDLL.dll` だけ)
  * 既定のフォントは Galmuri14 です。`make CE_FONT=shinonome` で東雲 16 ドット版(`AppMain_shinonome.exe`)、`make CE_FONT=galmuri11` で GalmuriMono11 版(`AppMain_galmuri11.exe`)も作れます。フォントは AppMain.exe に入っているので、フォントのファイルは要りません

```sh
git clone https://github.com/daizu-coder/PopSNES.git
cd PopSNES/CE
make
make strip
```

## 使用方法

対象は SHARP Brain(PW-G5300)。PC にリムーバブルディスクとして接続し、ドライブ直下に次の構成を作ります(メニュー項目名は機種により異なる場合があります):

```
<ドライブ直下>/
  アプリ/
    <任意のアプリ名>/
      AppMain.exe    ← ビルド生成物をそのまま
      index.din      ← 中身は空でよいダミーファイル
```

- ROM ファイルは SD カード上に置いてください。アプリ内の「Open ROM…」から選べます
- `index.din` をこの名前で置くと、そのフォルダが [追加アプリ・動画] に一覧表示されます
- 設定ファイル `popsnes.cfg` は、`AppMain.exe` と同じフォルダに作られます
- `popsnes_debug.log` は Video Config で「デバッグログを有効にする」を ON にしたときのみ生成されます(既定は OFF)

### 日本語フォルダ名について

ROM ファイル名・ROM を置くフォルダ名とも、日本語を含んでいても開けます(実機で確かめました)。snes9x2002 コアは元々 `fopen()` を一切呼ばず、ROM・セーブファイル・ステートともワイド文字 API(`_wfopen()`)のみで読み書きしているため、他の姉妹プロジェクトのような `fopen()` ラッパーは不要で、従来あった ASCII パス限定のチェックを撤廃するだけで対応できました。

## 動作確認環境

- SHARP Brain PW-G5300

PopGBA を CeOpener や CERestorer から起動すると、PW-G5300 で、終了後に画面が真っ暗になる、または操作を受け付けなくなることがありました。USB ケーブルと電池を抜いてから入れ直すと戻りました。PopSNES での動作は確かめていません。

## クレジット

- **snes9x2002** SNES エミュレーションコア — Snes9x 1.39〜1.43 系をもとにした PocketSNES / OpenSNES9X 系のフォーク(`src/`, `libretro/libretro.c`)。原作者は **Gary Henderson** 氏、**Jerremy Koot** 氏ほか。CPU / SPC700 / SuperFX / DSP-1 / C4 / S-DD1 / SA-1 の各実装に **Ivar**、**zsKnight**、**_Demo_**、**pagefault**、**John Weidman**、**Brad Jorsch**、**Kris Bleakley**、**Andreas Naive**、**neviksti**、**Nach** の各氏ほか多数が関わっています(<http://www.snes9x.com>)。DSP-1 の `src/dsp1emu.c` は **ZSNES Team**(GPL バージョン2以降)、SPC700 の ARM アセンブリ `src/spc700a.S` は **notaz** 氏(bitrider 氏が改変)の作です
- **libretro / snes9x2002** — 上記を libretro 化したもの。**Daniel De Matteis** 氏および **libretro チーム**が保守(<https://github.com/libretro/snes9x2002>)
- **libretro API** のヘッダ・**libretro-common** の一部 — **The RetroArch team**。MIT
- **Galmuri** ビットマップフォント(メニューの既定の文字)— **Lee Minseo**(quiple)氏。SIL Open Font License 1.1
  <https://github.com/quiple/galmuri>
- **東雲(しののめ)16 ドットビットマップフォント** — メインデザイン **古川 泰之** 氏ほか、**The Electronic Font Open Laboratory(/efont/)**。実質パブリックドメイン
  <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けクロスコンパイラ(`arm-mingw32ce-*`)。プロジェクト創設・主要開発の **Danny Backx** 氏、および gcc / binutils をモダンな版(GCC 9.3.0)へ引き上げた cegcc-build / cegcc-mk の **Max Kellermann** 氏。本プロジェクトのビルドは後者を直接使用しています。**Pedro Alves** 氏ほか貢献者の皆さんにも
- **SHARP Brain homebrew コミュニティ** — Brain 上でのアプリの作り方、`index.din` の仕組み、画面出力・音声・タスクバー制御など、端末固有の情報を Wiki やフォーラムに残してくださった皆さん
- Windows CE フロントエンド(`CE/`)は本プロジェクトで作成

コンポーネントごとの出所とライセンスの詳細は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) と [`LICENSING.md`](LICENSING.md) をご覧ください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

製作者がしたのは、AI への指示と、実機(PW-G5300)での確認です。アプリが途中で異常終了したときは、例外の内容を記録する [Brain Exception Reporter(BER)](https://github.com/Sgch/ber) を使いました。電源が落ちたときなどは、AI にデバッグ用の記録(ログ)を残す仕組みを作ってもらいました。そのうえで同じ操作をもう一度試し、記録を AI に読んでもらって原因を調べ、直してもらうことを繰り返して作りました。作る中で実機でわかったことは、[NOTES.md](NOTES.md) にまとめています。

マスコットの絵とアイコン(`CE/icon/`)は CC0 1.0(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。各ライセンスについては、[`LICENSING.md`](LICENSING.md) をご覧ください。

## ライセンス

PopSNES 全体は、上流の snes9x2002 と同じ条件(オリジナルの Snes9x の非商用ライセンス。[`LICENSE`](LICENSE) の3節)で配布します。上流のファイルのライセンスはそろっていないため(DSP-1 の `src/dsp1emu.c` は ZSNES Team の GPL バージョン2以降で、ライセンスの表記がないファイルもあります)、これ以上は断定しません。詳しくは [`LICENSING.md`](LICENSING.md) をご覧ください。

- **非商用に限ります。** 販売、商用の製品や活動での利用、有料での配布はできません
- 配るときは、ライセンスの文と各ファイルの著作権表示を残してください
- 「フリーソフト」「オープンソース」と書くときは、非商用に限ることも書いてください
- 保証はありません

PopSNES の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE) の2節)。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。

## 商標・免責

PopSNES は非公式のファンプロジェクトです。シャープ株式会社、任天堂株式会社とは関係がなく、許諾・後援も受けていません。

- 「SHARP」「Brain」はシャープ株式会社の商標です
- 「Super Nintendo Entertainment System」「Super NES」「スーパーファミコン」は任天堂の商標です

ゲームの ROM は含みません。利用者が合法的に入手したものを用意してください。
