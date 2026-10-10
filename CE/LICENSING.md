# PopSNES のライセンス

PopSNES は、スーパーファミコンのエミュレータ [snes9x2002](https://github.com/libretro/snes9x2002)(Snes9x 1.39〜1.43 系をもとにした PocketSNES / OpenSNES9X 系の libretro 版)を、SHARP Brain PW-G5300(Windows CE)向けに移植した、**非公式・非商用**の改変版です。Snes9x や snes9x2002 の作者やメンテナーはこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: 上流と同じ条件(Snes9x の非商用ライセンス)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、上流の snes9x2002 と同じ条件、つまりオリジナルの Snes9x のライセンスで配布します。本文は [`LICENSE`](LICENSE) の3節と [`LICENSES/LicenseRef-Snes9x.txt`](../LICENSES/LicenseRef-Snes9x.txt) にあります(コアの多くのソースの先頭に書かれている文と同じです)。

全体のライセンスを、これ以上はここで断定しません。上流の全ファイルを確かめたところ、書き方がそろっていないためです。

- `AppMain.exe` に入るファイルのうち、DSP-1 の `src/dsp1emu.c`(`src/dsp1.c` が `#include` で取り込むもの)は ZSNES Team の作で、**GNU GPL バージョン2以降**です。上流でもこの形で入っています
- `src/mode7*.c`、`src/rops.c`、65c816 の ARM アセンブリ(`src/os9x_65c816_*.S`、`src/os9x_asm_cpu.c`)、`src/spc_decode.S`、`libretro/libretro_core_options.h` など、ライセンスの表記がないファイルもあります
- libretro API のヘッダと libretro-common の一部は MIT です

それぞれの表記と許諾文の本文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています(GPL バージョン2の全文も付けています)。

Snes9x のライセンスの条件は次のとおりです。

- **販売しないこと。商用の製品や活動に使わないこと**(「Snes9x is freeware for PERSONAL USE only」「for non-commercial purposes」)
- 配るときは、ライセンスの文と著作権表示を、すべての複製と派生物に付けること
- 保証はありません

libretro 版では、Daniel De Matteis 氏の「UNDER NO CIRCUMSTANCE WILL COMMERCIAL RIGHTS EVER BE APPROPRIATED TO ANY PARTY」という一文も加わっています。

- PopSNES を売ること、売っているものに付けること、お金を払った人だけに配ることはできません
- リポジトリへの寄付やスポンサーは、ソフトを売ることではないので通常は問題ありません。ただし「お金と引き換えに機能を作る」ような形は、商用の活動に近くなるので避けてください
- 「フリーソフト」「オープンソース」と書くときは、非商用に限ることも書いてください(OSI や FSF の定義するフリーなライセンスではありません。GPL でもありません)
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリ)も入手できるようにしてください

非商用で配ることは Snes9x のライセンス文そのものが認めているので、作者への個別の確認はしていません。

## 2. snes9x2002 本体(上流のファイル)

リポジトリ直下のファイル(`src/`、`libretro/` など)は、上流の [libretro/snes9x2002](https://github.com/libretro/snes9x2002) のコミット `6ffbf9e`(2026-09-21)を元にしています。

PopSNES が変えた上流のファイルは次のものだけです。どれも Windows CE 用のツール(cegcc)とこの端末に合わせるため、またはコアの不具合を直すためのもので、ライセンスは変わりません。

- `libretro/libretro.c`: `Settings.SDD1Pack = TRUE` を設定した(S-DD1 の DMA が、中身が空のままの `S9xLoadSDD1Data()` に頼る経路を通り、圧縮されたままのデータを VRAM に送っていたため。起動したときのロゴなど)
- `src/os9x_65c816*.S`: `.include` のファイル名の大文字・小文字を実際のファイル名(`.S`)に合わせた(大文字と小文字を区別するファイルシステムでアセンブルできなかったため)
- `src/spc700a.S`: TCALL / BRK のベクタを、2バイトの読み込みではなく1バイトずつ読むようにした(そろっていない場所にあるため、この端末で止まっていた)
- `src/soundux.c`、`src/soundux.h`、`src/apu.c`: 音の処理を実機の DSP に合わせた(ノイズの作り方、ループ時の SRCN の読み直し、BRR の展開の計算)
- `.gitignore`: PopSNES のビルドの生成物などを除外するように書き換えた。上流に入っていた Visual Studio の作業ファイル `.vs/slnx.sqlite` は削除した

`LICENSES/LicenseRef-Snes9x.txt` は PopSNES が足したファイルで、Snes9x のライセンスの全文を入れています。

上流のソースの著作権表示は、変えずに残しています。

## 3. PopSNES の自作部分: MIT

PopSNES の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) の2節にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`Makefile`、`compat/`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - マスコットの画像とアプリのアイコン(下の「マスコットの絵とアイコン」を参照)

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。snes9x2002 と組み合わせて配布する場合は、1 の条件も守る必要があります。

## 4. マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`CE/icon/popsnes_mascot.bmp`)、アプリのアイコン(`CE/icon/popsnes.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/popsnes_mascot_C_osanpo_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。

README のスクリーンショット(`.github/screenshots/`)は、lunoka 氏のゲーム「Mai Nurse」を作者の許可を得て掲載しているもので、このリポジトリのライセンスの対象外です。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| snes9x2002 本体(`src/`)、libretro の接続部分(`libretro/libretro.c`) | Gary Henderson 氏、Jerremy Koot 氏ほか(Snes9x)、Daniel De Matteis 氏ほか(libretro) | Snes9x のライセンス(非商用。表記のないファイルもあります) |
| DSP-1 のエミュレーション(`src/dsp1emu.c`) | ZSNES Team | GNU GPL バージョン2以降 |
| libretro API のヘッダ、libretro-common の一部 | The RetroArch team | MIT |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

コアの中の各部分(SPC700 / APU、65c816 の ARM アセンブリ、Super FX、DSP-1、C4、S-DD1、SA-1、S-RTC)の作者は、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) の1節に載せています。

MIT や GPL は商用も認めていますが、PopSNES 全体の非商用の条件をゆるめるものではありません。

## 6. 同梱していないもの

- ゲームの ROM。任天堂の著作物などは含みません。使う人が自分で用意してください

## 7. 上流のファイルについての注意

- `src/` のうち ARM 以外向けの描画(`ppu_.c`、`gfx.c`、`tile.c`)や、使っていない特殊チップのコード、`jni/`、直下の `Makefile`、`.travis.yml`、`.gitlab-ci.yml` などは上流のファイルです。PopSNES のビルドには使っていません(`AppMain.exe` には入っていません)。ビルドに使うファイルは `Makefile.common` と `CE/Makefile` で決まります
- `libretro/libretro-common/include/compat/msvc/stdint.h`(Alexander Chemeris 氏、BSD-3-Clause)は MSVC 用で、PopSNES のビルドでは使っていません
- `src/dsp1emu_fixed.c`、`src/dsp1emu_yo.c`(ZSNES Team、GPL バージョン2以降)と `src/misc.S`(notaz 氏、ライセンスの記載なし)は、PopSNES のビルドでは使っていません
- リポジトリ直下の `README.txt` は上流の snes9x2002 の説明で、PopSNES の説明ではありません
