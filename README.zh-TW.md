# agent-terminal

[English](README.md) | **繁體中文**

一個以 C 語言寫成的輕量 tmux 式 session 多工器,專為讓長時間運行的終端機 AI
agent(例如 Claude Code CLI)在終端機前端崩潰後存活而生。支援 macOS 與
Linux,除 libc 外零依賴,MIT 授權。

> **正在用 coding agent?** 讓它讀 [AGENTS.md](AGENTS.md) — 與本 README
> 相同的內容,但為機器閱讀而寫:精確指令、實測 exit code、系統不變量,以及
> 常讓腳本呼叫者踩坑的非互動使用注意事項。

## 為什麼需要它

Session 狀態(PTY、螢幕、scrollback)通常與負責渲染它的行程活在一起。當終端
機模擬器在巨大的對話下掛掉 — 這是動輒數小時的 AI agent session 常見的死法 —
session 就跟著陪葬。agent-terminal 把兩者拆開:

- **`agent-terminald`** — 每使用者一個的 daemon,持有 PTY、模擬的螢幕狀態,
  以及持久化到磁碟的 scrollback。它不會隨前端一起死,而且 `reload` 能在活的
  session 底下原地 re-exec 自己 — 升級二進位檔的同時保住每一個子行程。
- **`agent-terminal`** — 跑在任何終端機裡的薄客戶端(Terminal.app、iTerm2、
  Ghostty、SSH 之上),經 unix socket 連上 daemon 並負責渲染。殺掉客戶端 —
  或強制退出整個終端機 — 再重新 attach:螢幕、游標、終端機模式精確還原,
  子行程毫無察覺。分割窗格、scrollback 翻頁、多客戶端 attach 都走同一條
  socket。

<p align="center">
  <img src="docs/architecture.svg" alt="架構圖:終端機內的薄客戶端經 unix socket 與 agent-terminald 溝通。客戶端可以任意崩潰;daemon 持有 PTY、VT 螢幕狀態與磁碟上的 scrollback,並存活下來。動畫依序演示正常運作、終端機死亡、daemon 繼續運行、新客戶端 attach 後由快照重繪。" width="900">
</p>

<sub>此圖在支援 SVG 的瀏覽器中會動起來(GitHub 支援)。四個階段:正常運作 →
宿主終端機死亡 → daemon 帶著仍在運行的子行程繼續 → 新客戶端 attach,快照精確
還原螢幕、游標與模式。</sub>

## 安裝

### 從原始碼

需求:C17 編譯器(clang 或 gcc)與 make。僅此而已。

```sh
git clone https://github.com/timwukp/agent-terminal.git
cd agent-terminal
make                   # release 建置 → build/release/
make test BUILD=asan   # 可選:在 ASan/UBSan 下跑單元測試
sudo make install      # 安裝到 /usr/local(可用 PREFIX= 覆寫)
```

安裝產物:`agent-terminald`、`agent-terminal`、`agent-terminal(1)` man page,
以及下面那兩個服務單元檔——它們會依你實際安裝的 `PREFIX` 產生,放在
`PREFIX/share/agent-terminal/`。

`PREFIX` 會先被正規化成絕對路徑,所以 `PREFIX=~/.local` 與相對路徑
`PREFIX=out` 都可以。這不是美觀問題:launchd 與 systemd 不做任何展開、而且
只接受絕對路徑,而 shell 對波浪號的規則參差不齊,足以讓二進位檔裝到正確位置、
單元檔卻寫著一個服務管理器啟動不了的路徑。若 `HOME` 未設而波浪號無法展開,
建置會直接停下,而不是產生一個說謊的單元檔。

如果另一個 prefix 底下已經裝了**不同的** `agent-terminald`,`make install`
會出聲告知。兩份副本在安裝時不會衝突,衝突發生在連線時,而且那時沒有任何
錯誤訊息——見[以服務方式運行 daemon](#以服務方式運行-daemon建議)。

### 以服務方式運行 daemon(建議)

daemon 會在首次使用時自動啟動,但交給服務管理器可以在崩潰與重開機後自動
重啟,讓 `attach` 永遠有效。請複製 `make install` 產生的那一份——它寫的是
你安裝的 prefix;`contrib/` 裡的副本是模板,刻意無法直接運行:

**macOS(launchd):**
```sh
cp "$PREFIX/share/agent-terminal/dev.agentterminal.daemon.plist" ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/dev.agentterminal.daemon.plist
```

**Linux(systemd user unit):**
```sh
mkdir -p ~/.config/systemd/user
cp "$PREFIX/share/agent-terminal/agent-terminald.service" ~/.config/systemd/user/
systemctl --user enable --now agent-terminald
loginctl enable-linger $USER   # 登出後 session 仍然存活
```

單元檔是產生的、而不是直接附上一份可複製的,因為裡面寫死一個路徑造成的不只是
不方便。指向你已經不用的 prefix 的服務單元,啟動的是還留在那裡的**舊**二進位
檔,而那個 daemon 會先應答 socket——於是客戶端的自動啟動(只在沒有任何 daemon
應答時才會發生)根本輪不到跑起來。協定會跳過不認識的訊息,所以舊 daemon 之後
才有的每一則訊息都變成靜默的空操作:新的按鍵綁定毫無反應,而且哪裡都不會印出
錯誤。**過期的 daemon 就是沒有打上補丁的 daemon。**想知道自己實際在跟哪一個
對話,用 `agent-terminal version`。

## 使用方式

### 快速開始

```sh
agent-terminal new -s work -- claude    # 在受管 session 中執行 claude
```

照常工作。若終端機崩潰,開一個新的然後:

```sh
agent-terminal attach -s work           # 一切都還在
```

### 指令

| 指令 | 效果 |
|---|---|
| `agent-terminal new [-s 名稱] [-- 指令 參數...]` | 建立 session 並 attach。預設名稱 `main`,預設指令 `$SHELL`。 |
| `agent-terminal attach -s 名稱` | attach 到運行中的 session。 |
| `agent-terminal ls` | 列出 session(尺寸、pid、attach 中的客戶端數)。 |
| `agent-terminal history -s 名稱` | 把 scrollback 傾印到 stdout。**daemon 不在也能用**,對已結束的 session 也有效。可接 `less -R`。 |
| `agent-terminal kill -s 名稱` | 終止 session。 |
| `agent-terminal reload` | 原地 re-exec daemon 以載入新的二進位檔。session、螢幕與 scrollback 全數保留;pid 不變。attach 中的客戶端會自行重連,新的映像也會從磁碟上的 log 重建每個窗格記憶體中的 scrollback ring,所以 reload 之後客戶端仍可往上捲動歷史(ring 最近的 10,000 行;`history` 仍讀完整的 log)。 |
| `agent-terminal version` | 客戶端建置版本(git hash),加上運行中 daemon 的 pid、重啟世代數,以及是否支援窗格。daemon 不在也能用(顯示 `daemon: not running`)。 |

Session 名稱會成為 `~/.agent-terminal/sessions/` 下的目錄,所以必須是單一
路徑組件:不可含 `/`、不可以 `.` 開頭、最長 63 bytes。它同時也是你在**終止
session 之前**會讀的標籤,因此必須是合法 UTF-8,且不可含看不見、或會改變周
圍文字順序的字元。這不是理論問題:在瀏覽器引擎中實測,名為
`proj<U+202E>gol.hs` 的 session 讓 GUI 的終止確認 `Kill proj<U+202E>gol.hs?
Its child process ends.` 實際算繪成
`Kill proj.sdne ssecorp dlihc stI ?sh.log`;而 `deploy` 與 `deploy<U+200B>`
量到的寬度精確相同 —— 兩列你分不出來,其中一列會把你的按鍵送到別的地方。

中間的點(`build_2026.08`)與任何真實文字(`日本語`、`專案-A`、希伯來文、
阿拉伯文、西里爾文、單一 emoji)都沒問題。zero-width joiner 與 variation
selector 不行,所以由序列組成的 emoji 不能當 session 名稱 —— 這是刻意的取
捨:在這裡「兩個名稱能被分辨」比「名稱能用 emoji 拼出來」更重要。無效名稱
以 exit 1 拒絕。由普通字母構成的相似字(用西里爾文 `е` 冒充拉丁文 `e`)**不
會**被攔下;GUI 的終止確認會在名稱旁顯示 **pid**,那是相似字唯一無法冒充的
東西。詳見 [SECURITY.md](SECURITY.md)。

名稱送上螢幕前,GUI 會再過濾一次 —— 改變順序的字元直接刪除,看不見的字元
顯示為 `U+FFFD`,好讓冒充者無法算繪成它所冒充的那個名稱。理由是 GUI 與
daemon 各自更新,較舊的 daemon 會接受現在會被拒絕的名稱。而用來**定位**
session 的字串,永遠是它真正的位元組。

### 窗格(Panes)

`Ctrl-\ "` 與 `Ctrl-\ %` 分割目前窗格(上下與左右);每個窗格是獨立的子行程,
擁有自己的螢幕與自己的 scrollback 歷史。合成畫面與分隔線由 daemon 繪製,所以
崩潰後 reattach 能精確還原整個分割狀態,連舊版客戶端也能正確顯示分割的
session — 合成好的畫面走的是一般輸出路徑。輸入、滑鼠回報與 bracketed paste
跟隨**目前作用中**的窗格。

### 按鍵綁定

- **`Ctrl-\` 接 `Ctrl-d`** — detach,session 繼續運行。單獨按下的 `Ctrl-\`
  會在 500 毫秒後轉送給應用程式,所以這組前綴鍵不會偷走這個按鍵。
- **`Ctrl-\` 接 `[`** — 進入 copy-mode,不必 detach 就能翻閱 scrollback。
  session 照常運行;翻頁期間產生的輸出會被略過,離開時由 daemon 的新快照
  重繪。有窗格時,copy-mode 顯示**作用中**窗格的歷史。
- **`Ctrl-\` 接 `"`** — 上下分割目前窗格。
- **`Ctrl-\` 接 `%`** — 左右分割目前窗格。
- **`Ctrl-\` 接 `o`** — 聚焦下一個窗格;**`;`** 上一個(最近使用的);
  **`x`** 關閉目前窗格。關掉最後一個窗格即結束 session。窗格小於
  2×20 欄 / 2×3 列加一條分隔線時,分割會被拒絕(附錯誤訊息)。
- **`Ctrl-\` 接方向鍵** — 聚焦該方向上最近的窗格(正對優先於斜角)。
  該方向沒有窗格時不做任何事。
- **`Ctrl-\` 接 `z`** — 縮放(zoom):目前窗格暫時佔滿整個畫面;再按一次
  `z`(或任何分割/關閉/焦點切換)即還原版面。`ls` 會標示縮放中的 session。

Copy-mode 內:

| 按鍵 | 動作 |
| --- | ------ |
| `j` / `k`、`↓` / `↑` | 一行 |
| `Space` / `Ctrl-f`、`Ctrl-b`、`PgDn` / `PgUp` | 一頁 |
| `Ctrl-d` / `Ctrl-u` | 半頁 |
| `g` / `G`、`Home` / `End` | 最舊 / 最新一行 |
| `/`*樣式*,然後 `n` / `N` | 向前 / 向後搜尋(不繞回) |
| `q` / `Esc` | 離開 copy-mode |

Copy-mode 顯示的是已**捲出**螢幕的內容,所以在 24 列的終端機裡印了 200 行的
session 提供 177 行歷史 — 其餘 23 行還在螢幕上。搜尋比對的是你看到的文字,
不含周圍的色彩跳脫序列。底部列顯示位置與總數。

### 腳本 / 非互動使用

`agent-terminal new -s x -- cmd < /dev/null` 會建立 session,並在 daemon
確認後以 0 退出;session 以零客戶端狀態持續存在,隨時可 `attach`。客戶端
只回報 daemon 確認過的事:對不存在的 session 執行 `attach`,即使 stdin 已達
EOF 也會以 `no such session`、rc=1 失敗;daemon 5 秒內未確認任何事時,指令
會失敗而不是吊住腳本。若要從腳本*操作* session(輸入內容、觀察輸出),請保持
stdin 開啟 — FIFO 寫法見
[AGENTS.md §4](AGENTS.md#4-scripted--non-interactive-use)。

### SSH session

```sh
agent-terminal new -s prod -- ssh user@host
```

這是在 session 裡執行你**系統上的 OpenSSH 客戶端** — 它繼承
`~/.ssh/config`、known_hosts 與你的 SSH agent。agent-terminal 本身不含任何
SSH 或密碼學程式碼。

### Session 環境

session 內的程式看到的是 `TERM_PROGRAM=agent-terminal`——而不是 daemon
當初碰巧從哪個終端啟動;殘留的 `TERM_PROGRAM_VERSION` / `TERM_SESSION_ID`
會被移除。依終端身分調整行為的程式(Claude Code 就是)否則會去適配一個
它根本沒在對話的終端;看到陌生身分時它們會退回可攜的通用行為,而那正是
中間這層 VT 引擎實際支援的。(tmux ≥ 3.2 也是這麼做的。)

### 典型工作流

```sh
# 一個必須撐過任何狀況的長時 AI agent session:
agent-terminal new -s agent -- claude

# 崩潰後(即使是 daemon 自己崩潰)找回歷史:
agent-terminal history -s agent | less -R

# 多個平行 session:
agent-terminal new -s build -- make -j8
agent-terminal new -s logs  -- tail -f /var/log/system.log
agent-terminal ls

# 或者改用分割:agent 在左窗格,
# Ctrl-\ % 之後在右邊 tail 日誌。
agent-terminal new -s work -- claude
```

## GUI 客戶端(`app/`,早期預覽)

桌面客戶端位於 [`app/tauri`](app/tauri) — Tauri + xterm.js,走的是同一個 Unix
socket,沒有新增任何網路監聽。attach 時會回填 daemon 側的歷史紀錄(最多為
ring 的 10,000 行),所以滑鼠滾輪能一路捲回 GUI 連上**之前**的輸出。⌘/Ctrl
`+` `−` `0` 縮放字級,不會改變 session 的格線;視窗永遠不會擅自調整**別人
建立的** session 尺寸——工具列的 ⤢ 按鈕(或標示大片留白、標示視窗小於格線的
那個提示)才會,且所有 attach 中的檢視端一起重排;GUI 自己建立的 session
則在誕生時就貼合視窗一次。
右側可收合面板顯示 Claude Code 的即時狀態,讀自 `~/.claude`(終端協定
本身保持與工作負載無關):**Usage** 分頁是每份 transcript 的輸入/輸出/
快取 token 總量與每分鐘輸出 sparkline;**Hooks** 分頁列出 `settings.json`
裡的 hook 規則,附唯讀的腳本檢視器(GUI 永不寫入 Claude Code 的設定;
檢視器只讀已設定的 hook 指名的那個路徑,且該路徑當下仍必須是普通檔案 —
腳本原地被換成 symlink 時是拒絕,而不是跟著走 — 並且最多讀 1 MiB,
截斷時會在畫面上說明),以及一張安全卡片,尾隨選擇性啟用的雜湊鏈 hook 執行日誌
(`app/design/hook-log.md`——設計上可偵測竄改,並誠實標明不防竄改)。
它**不**屬於 `make install` 流程;由你自行編譯,可以打包成可雙擊的 app,
也可以只建 debug 執行檔:

```sh
cd app/tauri && npm ci
npm run bundle       # → src-tauri/target/release/bundle/macos/agent-terminal.app
```

`.app` 的意義不只是方便:**macOS 只為真正的 `.app` bundle 顯示 OS 通知**,
所以完成通知的彈窗只存在於打包版。它是 ad-hoc 簽章——本機執行沒問題,但
不能發佈。想放進 Launchpad 就拖到 `/Applications`。

開發時用裸執行檔重建比較快:

```sh
cd app/tauri
npm ci && npm run build          # Tauri 在編譯期把 dist/ 嵌入二進位檔
cd src-tauri && cargo build      # ./target/debug/agent-terminal-gui
```

這個順序不是建議,`cargo build` 現在會強制執行:因為前端 bundle 是在編譯期
嵌入的,只建置 Rust 那一半會讓新指令配上舊 JavaScript — 看起來像應用程式壞了,
而不是建置壞了(曾經因此拒絕了每一次按鍵)。當 `dist/` 不存在、或比 `src/`
更舊時,build script 會直接失敗並附上修復指令。

把前端嵌進二進位檔還有一個後果:這個 app 的 **Content Security Policy 是會影響
功能的設定,不是裝飾性的加固**。macOS 上 Tauri 的 IPC 是一個發往
`ipc://localhost/<command>` 的 `fetch`,而頁面本身由 `tauri://localhost` 提供
— 兩者 scheme 不同 — 所以 `connect-src` 若少了 `ipc:` 就會被擋掉。Tauri 接著
會退回 `postMessage`,而那條路徑會把 payload 做 JSON 編碼,因此**無法**傳遞
原始位元組。唯一會傳原始位元組的指令是終端機輸入,所以症狀非常特定:渲染、
側邊欄、切換 session 全都正常,只有鍵盤是死的。有一個單元測試會確認實際出貨的
policy 保留了那個 scheme。

目前可用的功能:側邊欄列出活的 session,含窗格數、zoom 標記與客戶端數量
(與 `ls` 同一份資料,輪詢取得);點擊即 attach 並渲染;一鍵範本建立新的
Claude 或 shell session;結束 session(右鍵,並且會先詢問——詢問的視窗是 app
自己畫的,因為平台的 `confirm()` 對話框在這個 webview 裡是死的按鈕,所以這裡
唯一的破壞性操作不依賴它);鍵盤輸入;在分割中點擊切換焦點窗格;
分割/縮放/關閉窗格的工具列;以及作用中窗格的外框標示,讓分割的 session
一眼看出輸入會進到哪裡。

GUI 也會在 session **於你不在時完成長任務**時通知你:兩個觸發條件取聯集 —
終端機響鈴(單一窗格用 xterm 的解析器,所以 OSC 標題寫入不會誤觸;分割
session 走 daemon 的 `MSG_PANE_BELL`,因為合成畫面會剝掉原始的 `\a` — 兩條
路徑互斥,鈴聲絕不會響兩次),以及 Rust 核心
裡的輸出閒置狀態機(持續輸出 ≥10 秒後靜默 ≥5 秒;一行 `ls` 永遠不會通知)。
視窗在前景時絕不通知——你本來就在看。每個 session 列有 🔔/🔕 開關;靜音只
關掉彈出通知,該列仍會出現 ✓ 標記——這也涵蓋了 OS 通知根本送不出去的情況
(macOS 只為真正的 `.app` bundle 顯示通知,所以未打包的 debug binary 一律
退回 ✓ 標記)。

通知的**內文是 session 自己的最後一行螢幕輸出**,完全按照該程式寫出的樣子
傳遞 —— 請把它當成那個程式的「說法」,而不是事實;它永遠是文字,不是標記
也不是指令。**標題**裡的 session 名稱則會過濾,因為我們自己的 `— finished`
接在它後面:在瀏覽器引擎中實測,`proj<U+202E>gol.hs — finished` 會算繪成
`projdehsinif — sh.log`,連應用程式自己的字都被反轉並塞進名稱中間。過濾後,
它由左至右算繪,且名稱的每個字形都完整保留。

一個值得記錄的實作細節:協定的 layout 訊息沒有 zoom 欄位,我們也沒有新增。
窗格縮放時,daemon 本來就會把該窗格的矩形回報為整個畫面(其他窗格保持
原本的並排矩形),所以 GUI 是從它本來就會收到的幾何資訊**推導出** zoom
狀態。這個推論依賴另一個元件的實作細節,因此有一個整合測試對真實 daemon
驅動 split/zoom/unzoom,幾何契約一旦改變就會大聲失敗。

有兩點值得明說,因為很容易想錯:

- **它與 CLI 並存。** 多客戶端 attach 是 daemon 原生能力,所以終端機裡的
  `agent-terminal attach -s work` 與顯示 `work` 的 GUI 會同時即時渲染同一個
  session,彼此不會搶走對方的輸入。
- **它是檢視器,絕不改變你的 session 尺寸。** Session 幾何是所有客戶端共用的
  持久 session 狀態,所以一個把自己視窗尺寸套上去的客戶端,會在別人 attach
  的情況下把正在跑的 TUI 重排。GUI 以「保持你目前的尺寸」的 sentinel 來
  attach,並且**以 1:1 繪製**格線:格線比視窗小就留白,比視窗大就裁切並可
  捲動,而且一開就固定在最下面一列(提示符所在的位置)。調整視窗大小對遠端
  不造成任何影響。它刻意**不**把畫面縮放進視窗——一個被視覺縮放過的終端機,
  會把**錯誤的儲存格**回報給裡面正在跑的程式(實測:0.6× 之下點第 40 欄,
  傳出去的是第 25 欄,於是啟用滑鼠的 `vim` 或 `less` 會對錯誤的那一行動作,
  而畫面上看起來一切正常)。想讓 session 貼合視窗,請按 ⤢。

視窗有**兩種外觀,兩種都是設計出來的**——淺色不是深色的反轉。每一組元件真正
會畫出來的「前景色 × 背景色」配對,都由一個會計算對比值的測試依 WCAG 2.1 檢查
(文字 4.5:1、邊框與 sparkline 3:1),而不是靠眼睛判斷;正是這個量測抓到深色
主題自己的錯誤色只有 4.21:1。淺色另外重畫全部 16 個 ANSI 色槽,因為 xterm 的
預設值本身就是為深色背景挑的——它的 `white` 在白色終端上只有 1.46:1,也就是你
根本讀不到的 session 輸出。側邊欄底部的控制項提供 *system*、*light*、*dark*;
選 *system* 時視窗會即時跟隨 macOS 外觀,明確指定的選擇則會在重新啟動後保留。

尚未實作:拖曳分隔線調整窗格大小(需要新的協定訊息),以及 token 用量、
hooks 與安全性面板 — 這些 crate 目前還只是骨架。驗收採用
[docs/UAT.md](docs/UAT.md#gui-client-apptauri--manual-checklist) 中的手動
檢查清單;協定層行為則由 `app/tauri/src-tauri/crates/at-client/tests/` 下的
真實 daemon 整合測試覆蓋。

## Scrollback 持久化

捲出主螢幕的行會保存在 1 萬行的記憶體環形緩衝區中,並附加到 CRC 框定的磁碟
日誌(`~/.agent-terminal/sessions/<名稱>/scrollback.log`,2×32 MiB 輪替)。
日誌撐得過 daemon 崩潰 — 復原時會在第一筆損毀記錄處截斷 — 且 `history` 直接
讀檔,不需要 daemon。記錄儲存的是渲染好的 ANSI 文字,所以直接
`less -R scrollback.log` 也能讀。

Session 結束時,仍在螢幕上的內容也會刷入日誌,所以一則從未捲出螢幕的簡短
崩潰訊息依然能用 `history` 找回。結束於替代螢幕(vim、htop)的 session 不會
刷入 — 日誌只保存主螢幕內容。

## 安全性

威脅模型與漏洞回報方式見 [SECURITY.md](SECURITY.md)。重點:

- Unix socket 位於 0700 目錄、權限 0600,並驗證對端 UID
  (`SO_PEERCRED` / `getpeereid`)。完全沒有任何網路監聽。
- **UID 就是整條信任邊界,所以 session 之間彼此並沒有隔離。** 任何通過 UID
  檢查的客戶端,對**每一個** session 都是完全授權的——也就是說,跑在
  session A 裡的指令可以連上 socket,然後讀取、輸入、或結束 session B。這是
  按協定設計本來就能做的事,不是利用什麼漏洞。tmux 與 screen 是同一個模型;
  這裡要明講出來,是因為「在 session 裡跑 agent」正是「所有以你身分執行的東西
  都同等可信」這個假設最容易跟人的直覺脫節的場景。彼此不該互通的工作負載,
  要用不同的 UID 或容器隔開,而不是用不同的 session。
- Session 指令走 `execvp`,**從不經過 shell**——整個 repo 裡沒有 `system()`
  也沒有 `popen()`——所以 argv 裡的特殊字元只會是字面上的引數位元組。daemon
  從不是 setuid。
- VT 解析器 — 不可信輸入的第一線 — 是隔離的、**無 syscall** 的函式庫
  (`src/vt/`),每夜以 libFuzzer 模糊測試,每個 PR 在 ASan+UBSan 下運行,
  並以真實 vttest 錄製檔做黃金重放一致性測試。
- 本 repo 不含密碼學程式碼:SSH 完全委派給系統的 OpenSSH。

## 限制

最初的 v1 版本記載了四個刻意保留的缺口 — 沒有 scrollback 翻頁、丟棄組合
字符、daemon 重啟殺死子行程、沒有窗格。四者現已全部實作。目前仍存在的:

- 沒有 windows/tabs — 一個 session 就是一個可見畫面。分割上限為每 session
  6 個窗格;小於 20×3 加一條分隔線的窗格會拒絕再分割。
- Daemon **崩潰**仍會殺死子行程。Daemon **重啟**已不會:`agent-terminal
  reload`(或 `SIGHUP`)讓 daemon 原地 re-exec,pid 不變,沒有任何 PTY
  master fd 被關閉,子行程看不到載波中斷的 `SIGHUP`,daemon 之後仍是每個
  子行程的父行程。螢幕與 scrollback 一併帶過;attach 中的客戶端自行重連。
  這正是在活的 session 底下升級二進位檔所需要的,而且有測試
  (`tests/integration/test_restart.sh`)。

  崩潰存活是另一個問題,**尚未**解決。`SIGSEGV` 或 `kill -9` 時我們的程式碼
  一行都不會執行,所以沒有東西能撐住 master fd — 而掛斷子行程的正是 fd 被
  關閉,不是 daemon 行程結束本身。要修這個,需要一個同時持有 fd 且身為子行
  程父行程的第二個行程,亦即倒轉整個架構,讓 supervisor 生 PTY、daemon 退化
  為無狀態的渲染器。刻意不做。scrollback 依然存活於磁碟,`history` 可以
  找回。

  `systemd` 使用者請注意:`systemctl --user restart` **不會**保留 session —
  停止 daemon 就會關閉它的 fd。請用 `reload`。隨附的 unit 也設定了
  `KillMode=mixed`,否則 `stop` 會對 cgroup 內的每個行程(包括子行程)發送
  `SIGTERM`,daemon 做什麼都攔不住;`setsid` 並不會離開 cgroup。
- 每格**一個**組合字符(combining mark),且僅限 BMP(U+0000–U+FFFF)。
  這涵蓋所有現代活文字;含兩個以上字符的字素簇保留第一個,增補平面的字符
  (古文字、音樂記譜)仍會丟棄。ZWJ 序列在此不算字素簇,所以多人 emoji 會
  拆成個別字形。CJK 全形字元完整支援。

## 疑難排解

**Daemon 有沒有在跑?哪個版本?**
```sh
agent-terminal version
# agent-terminal 1a2b3c4d5e6f
# daemon: pid 4242, generation 3, panes yes
```
`generation` 計數原地 reload 的次數(pid 刻意不變)。`daemon: not running`
不是錯誤 — 下一次 `new` 或 `attach` 會自動啟動它。

從改過的 checkout 建出來的版本會印 `<hash>-dirty.<8 位十六進位>`,後綴是
對樹的實際內容(含未追蹤檔案)取的雜湊 — 兩棵不同的改動樹絕不會共用同一個
版本字串,所以「我跑的是不是剛剛建的那份?」在開發期間也答得出來,不必等
到發佈。從 release tarball(沒有 `.git`)建出來的則印 `.tarball-version`
檔案裡記錄的發行版本。

**`new -s x -- 某指令` 「毫無反應」— session 沒出現在 `ls`?** 指令瞬間退出
了,最常見的原因是它**不在 daemon 的 PATH 上**:由服務管理器啟動的 daemon
只拿到極簡 PATH(launchd:`/usr/bin:/bin:...`),而 session 的指令繼承它。
失敗原因會寫在 session 的螢幕上並保存進 scrollback:
```sh
agent-terminal history -s x
# agent-terminald: exec 某指令: No such file or directory
# (daemon PATH: /usr/bin:/bin:/usr/sbin:/sbin)
```
修法:把指令所在目錄加進服務單元的 PATH — 見隨附 launchd plist 的
`EnvironmentVariables` 區塊,或 systemd unit 中註解掉的
`Environment=PATH=` 行 — 然後重載服務。直接寫絕對路徑
(`-- /完整/路徑/指令`)也可以。

**按鍵組合沒反應(分割、copy-mode)?** 幾乎都是版本偏差:一個較舊的 daemon
在回應 socket。未知訊息按設計會被跳過,所以新的按鍵組合對舊 daemon 是靜默
無效。`agent-terminal version` 能看出偏差(`panes no`,或 daemon 的 hash 比
客戶端舊),attach 時客戶端也會警告。不丟 session 的修法:
```sh
agent-terminal reload
```

**客戶端崩潰 / 終端機凍結?** Session 沒事。開一個新終端機執行
`agent-terminal attach -s <名稱>`。忘記名稱就 `agent-terminal ls`。

**Daemon 真的崩潰了?** Session 與其子行程已終止(見「限制」),但
scrollback 存活於磁碟:
```sh
agent-terminal history -s <名稱> | less -R
```
下一次 `new` 會啟動全新的 daemon;崩潰遺留的 socket 或鎖檔會被自動偵測並
清理。

**Session 從 `ls` 消失了?** 它的子行程結束了 — 這是 `ls` 的契約(結束的
session 直接消失,而非顯示「dead」)。最後的螢幕已刷入 scrollback,所以
`history -s <名稱>` 能看到它最後印出的內容,包括從未捲出螢幕的部分。

**腳本呼叫吊住或說謊?** 不應該:stdin 在 EOF 時客戶端會等待 daemon 確認,
對不存在的 session 以 `no such session`(rc=1)失敗,對卡死的 daemon 在 5 秒
後以 `no confirmation from daemon` 放棄。見
[AGENTS.md §4](AGENTS.md#4-scripted--non-interactive-use)。

## 測試

三個層次,`main` 上全綠:

- **單元測試**:9 個套件共 6,541 個檢查(VT 解析器逐位元組、協定往返與
  違規、環形緩衝區、scrollback CRC 復原、窗格版面幾何(含狀態檔可能帶進來的
  環狀樹)、輸入按鍵掃描器、翻頁器、路徑驗證、事件迴圈)— 在 ASan+UBSan
  下運行。
- **整合測試**:31 個端到端腳本,覆蓋本工具存在意義上的失效模式 — 客戶端
  `kill -9` 後 reattach、daemon reload 且子行程存活、daemon reload 後只走協定
  的客戶端仍能讀回 scrollback(讀檔案的測試對這個缺陷不可能紅)、經協定驅動的窗格分割
  / 方向導航 / 縮放、100 MB 記憶體上限 soak、畸形 handoff 狀態檔、路徑穿越
  探測、關閉競態、誠實的錯誤回報、安全稽核輪次找出的同 uid 濫用案例
  (被繼承的 scrollback fd、超大幾何、連上後從不表明身分的連線),以及
  可能留下舊 daemon 應答 socket 的安裝路徑。每個 PR
  在 macOS 與 Linux 的 CI 上運行。
- **真實 Claude Code session 的終端使用者驗收測試(UAT)** — 真實工作負載,
  以真實 pty 與真實按鍵驅動,兩輪加上五次稽核輪次追加的案例,共 22 個
  案例:崩潰後 reattach 且同一個
  行程繼續回答、活的 TUI 周圍做窗格方向導航與縮放、對話底下升級 daemon
  二進位檔、多客戶端旁觀、批次 `claude -p` 模式。完整測試日誌 — 案例編號、
  資料、步驟、判定、第一輪抓到的 macOS reload bug,以及 TUI 測試方法 — 見
  **[docs/UAT.md](docs/UAT.md)**。

## 開發

```sh
make BUILD=debug            # -O0 -g3
make test BUILD=asan        # ASan+UBSan 下的單元測試
make fuzz-regress BUILD=asan  # 重放 fuzz 語料庫(任何編譯器都能跑)
make fuzz BUILD=fuzz CC=clang # libFuzzer 二進位檔(需要 fuzzer runtime)
BUILD=release bash tests/integration/test_reattach.sh   # 驗收測試
python3 tools/check_svg.py docs/architecture.svg        # 架構圖幾何檢查
```

建置與測試的 `BUILD` 必須一致 — 整合腳本解析 `build/$BUILD`,二進位檔缺失
時會明確說明。

目錄配置:`src/vt/`(隔離的 VT 引擎)、`src/daemon/`、`src/client/`、
`src/common/`(協定、環形緩衝區、scrollback)、`tests/`、`fuzz/`。

貢獻指南:[CONTRIBUTING.md](CONTRIBUTING.md)。給 coding agent 的機器導向
參考:[AGENTS.md](AGENTS.md)。

## 授權

[MIT](LICENSE)。以**「現狀」**提供,不附任何形式的保證 — 保證與責任的完整
免責聲明見 LICENSE 檔案。
