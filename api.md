# primeLua API 参考

HP Prime G1 上跑的 **Lua 5.4.7** 解释器（`lua.elf`）对外提供的全部接口。
本文只讲 Lua 侧看得见的东西；移植细节、测试体系、真机排错见 `README.md`。

> **版本**：解释器 API 版本 `1.0`（`sys.version()` 取到，启动时也会写进 `plua.log` 的
> `interp:` 行）。**换脚本请连 `lua.elf` 一起换**：新脚本配旧解释器会出现
> `attempt to call a nil value (field 'xxx')` 这类"字段不存在"。

---

## 目录

| 章节 | 内容 |
|---|---|
| [0. 怎么用](#0-怎么用) | 全局表 / `require`、错误约定、数字与打印 |
| [1. 标准库](#1-标准库lua-547-原版) | base / coroutine / table / string / math / io / os / utf8 / debug / package，以及设备限制 |
| [2. `gfx`](#2-gfx屏幕与图形) | 屏幕、图元、5×8 文字、GROB 双缓冲、绘制目标 |
| [3. `font`](#3-font真字体) | Montserrat / Cascadia Code Light 真字体（16 / 24 / 32 px）+ 钟表用 56 px 数字面 `font.big()` |
| [4. `key`](#4-key键盘与触摸) | 按键钩子、扫描码、触摸 |
| [5. `sys`](#5-sys系统) | 版本、固件时钟、持久日志、内存、睡眠 |
| [6. `rand`](#6-rand随机与噪声) | PCG32 随机数、值噪声 |
| [7. `codec`](#7-codec编码校验) | CRC32 / Adler32 / hex / base64 |
| [8. `fx`](#8-fxq1616-定点) | Q16.16 定点（无 FPU 时更快） |
| [9. `dd`](#9-dddouble-double31-位十进制) | 31 位十进制高精度库 |
| [10. 键码表](#10-键码表) | 设备键码 vs PPL GETKEY |
| [11. 颜色与尺寸](#11-颜色与尺寸) | 调色板常量、屏幕几何 |
| [12. 例子](#12-例子) | 可直接跑的片段 |
| [13. 已知限制](#13-已知限制) | 语言、时间、IO、精度 |

---

## 0. 怎么用

每个硬件库都同时是**全局表**和 **`package.preload` 的一个加载器**，两种写法等价，
拿到的是同一张表：

```lua
gfx.clear(gfx.BLACK)                    -- 直接用全局
local gfx = require("gfx")              -- 或者 require（同一个表：require("gfx") == gfx）
local font, dd, sys = require("font"), require("dd"), require("sys")
```

`require` 只找 Lua 文件（没有 C 模块，`package.cpath` 为空），搜索路径：

```
C:\DATA\primeLua.hpappdir\?.lua;C:\DATA\primeLua.hpappdir\?\init.lua;C:\DATA\?.lua
```

**错误约定**：参数类型不对会抛 Lua 错误（`bad argument #N to 'f' ...`）；硬件层面的
失败则**返回状态**而不是抛错——`gfx.init()` 返回布尔，`key.install()` 返回布尔，
`codec.unhex` 对坏输入抛错，`dd` 的非法输入返回 `nan` 而不是抛错。用 `pcall` 包住
不确定的调用是常规做法。

**数字与打印**：Lua 的数字是 **64 位整数 + double**（`math.type(x)` 区分）。
`print`/`tostring` 按 Lua 5.4 的默认格式 **`%.14g`** 打印浮点（14 位有效数字，
桌面 Lua 一样）；要看满精度用 `string.format("%.17g", x)`，要精确位用 `%a`：

```lua
print(math.sin(1))                     --> 0.8414709848079
print(string.format("%.17g", math.sin(1)))  --> 0.8414709848078965
print(string.format("%a",    math.sin(1)))  --> 0x1.aed548f090ceep-1
```

`math` 本身与 glibc 同档（788 组输入实测最大 1.31 ulp，96.6% 与 Python 逐位相同）；
要 30 位以上用 [`dd`](#9-dddouble-double31-位十进制)。

---

## 1. 标准库（Lua 5.4.7 原版）

`_G`（`print type tostring tonumber pairs ipairs next select rawget rawset rawequal
rawlen setmetatable getmetatable pcall xpcall error assert load loadfile dofile
collectgarbage _VERSION`）、`coroutine`、`table`、`string`（含模式匹配、`pack/unpack`、
`dump`、`format`）、`math`、`io`、`os`、`utf8`、`debug`、`package` 全部可用，
行为与桌面 `lua` 逐字节一致（差分测试就是比这个）。编译时带 `-DLUA_COMPAT_5_3`，
所以 `math.log10` / `math.pow` 也在。

**设备上的差别**：

| 项目 | 说明 |
|---|---|
| `os.clock()` | 解释器起来以后过了多少**秒**（计算器只有一个进程，所以给的是墙上时间；单位由固件计数器现场标定，见 README §4.4）。没有时钟源时 `0` |
| `os.time()` / `os.date()` | 先读**固件实时钟**（svc 0x100A5），读不到再退回启动器每次运行前从 PPL 读一次的墙钟；两个都没有时 `time()` 返回 `0`（1970-01-01），不造假 |
| `os.getenv()` | 恒 `nil`（没有环境块），因此 `LUA_INIT` 不生效 |
| `os.execute` / `os.rename` | 无进程、固件未暴露 rename → 失败 |
| `os.tmpname()` | 固定在应用目录 |
| `package.cpath` | `""`，没有动态库，`require` 只加载 `.lua` |
| `io` | 走固件文件 API：路径可写相对名（会被前置到应用目录）或 `C:\...` 绝对路径；**模式已翻译**（`"w"`→`"wb+"`、`"a"` 保留旧内容再定位末尾、`"r+"` 保留内容），由 `port/plua_fwmode.c` 处理；无 `io.popen` |
| 输出 | 进 32 KB 环形缓冲，启动器**边跑边读**打印（1.0 起程序跑在固件线程上）；`io.write` 与 `print` 同路 |

---

## 2. `gfx`：屏幕与图形

### 绘制模型（先看这个）

| 语句 | 效果 |
|---|---|
| `gfx.init()` | 初始化（读固件 LCD 结构）。**绘制目标 = 屏幕本身**：画了就能看见，不需要 `blit`，也不分配内存 |
| `gfx.select()` | **按需分配** 307 KB 默认后缓冲，并用**当前屏幕内容做种子**；之后画进缓冲、`gfx.blit()` 上屏 |
| `gfx.select(false)` | 回到直接画屏幕 |
| `gfx.select(g)` | 画到某个 GROB（离屏合成） |
| `gfx.blit()` | 把默认后缓冲整屏拷上屏（没有后缓冲时是空操作） |
| `gfx.blit(g, x, y)` | 把 GROB 拷到**当前绘制目标**（不切目标） |
| `gfx.freebackbuf()` | 释放后缓冲（307 KB 还给固件堆），回到直接画屏幕 |
| `gfx.target()` | 返回 `"screen"` / `"default"` / `"grob"`，告诉你现在画到哪儿 |

写回缓冲时目标不会悬空：`gfx.freegrob()` 释放的正是当前目标时，会先切回默认后缓冲。

### 函数

| 函数 | 说明 |
|---|---|
| `gfx.init() -> bool` | 初始化；`false` 表示拿不到显示。每个脚本开头调用一次 |
| `gfx.width() -> n` | 屏幕宽（320） |
| `gfx.height() -> n` | 屏幕高（240） |
| `gfx.rgb(r, g, b) -> color` | 由 0–255 分量合成颜色（等于 `gfx.RED` 这类常量的编码） |
| `gfx.clear(color)` | 用颜色填满当前绘制目标 |
| `gfx.pixel(x, y, color)` | 画一个像素（越界自动忽略） |
| `gfx.getpixel(x, y) -> color` | 读当前绘制目标的像素 |
| `gfx.line(x1, y1, x2, y2, color)` | 直线 |
| `gfx.hline(x, y, w, color)` | 水平线，宽 `w` |
| `gfx.vline(x, y, h, color)` | 垂直线，高 `h` |
| `gfx.rect(x, y, w, h, color)` | 空心矩形 |
| `gfx.fillrect(x, y, w, h, color)` | 实心矩形 |
| `gfx.circle(cx, cy, r, color)` | 空心圆 |
| `gfx.fillcircle(cx, cy, r, color)` | 实心圆 |
| `gfx.triangle(x1,y1,x2,y2,x3,y3,color)` | 空心三角形 |
| `gfx.filltriangle(x1,y1,x2,y2,x3,y3,color)` | 实心三角形 |
| `gfx.text(x, y, s, color)` | 内置 **5×8 点阵**文字（ASCII 0x20–0x7F，固定 6 px 步进，一行 53 字符）。要真字体看 [`font`](#3-font真字体) |
| `gfx.textbg(x, y, s, color, bgcolor)` | 同上，先用 `bgcolor` 填文字框（任何背景上都看得清） |
| `gfx.textwidth(s) -> n` | 5×8 文字宽（`6 * #s`） |
| `gfx.blitpixels(data, w, h, x, y [, keycolor])` | 把 `string.pack` 出的 `w*h` 个小端 ARGB 字直接贴上去；给 `keycolor` 则该色透明 |
| `gfx.newgrob(w, h) -> grob` | 新建离屏位图（GROB），初始全黑；`grob` 是 userdata，被 GC 时自动释放 |
| `gfx.freegrob(g)` | 立刻释放（幂等） |
| `gfx.grobwidth(g) -> n` / `gfx.grobheight(g) -> n` | GROB 尺寸 |
| `gfx.select([g/false])` | 见上表 |
| `gfx.target() -> "screen"/"default"/"grob"` | 当前绘制目标 |
| `gfx.fb() -> addr, w, h` | 固件 LCD 结构里的帧缓冲地址与尺寸（**排错用**：整屏绘制出问题时先看这里） |
| `gfx.freebackbuf()` | 释放默认后缓冲 |

### 常量

`gfx.BLACK WHITE RED GREEN BLUE YELLOW CYAN MAGENTA ORANGE GRAY`、
`gfx.WIDTH`（320）、`gfx.HEIGHT`（240）。

### 性能与注意事项

- 每帧都 `gfx.blit()` 是**一次整屏拷贝**（约 77k 次写入），比"画一条擦一条"流畅得多。
- **不要在帧循环里调 `sys.memory()`**（见 [5](#5-sys系统)）。
- 图形程序跑的时候 Python 侧被同步调用阻塞，但**屏幕照常刷新**（Lua 自己 blit，
  固件 LCD 一直在扫），所以能看着画面长出来。
- 屏幕几何是固定的 320×240；`gfx.width()/height()` 读的是固件给的值。

---

## 3. `font`：真字体

primetcc 的字体引擎 + **两族正文，各 16 / 24 / 32 px**，另有一个**钟表专用的大数字面**
（4-bit 抗锯齿）：

| 族 | 取法 | 字形 |
|---|---|---|
| 比例 | `font.prop([size])` | Montserrat |
| 等宽 | `font.mono([size])` / `font.cascadia([size])` | Cascadia Code Light（两个字是同义：等宽格，`":"` 与数字同宽） |
| 大数字 | `font.big()` | Cascadia Code Light **原生 56 px**，只含 `'-' '.' '/' '0'-'9' ':'` |

```lua
local h = font.prop()                 -- 或 font.mono() / font.cascadia()
font.draw(h, 10, 20, "Hello", gfx.WHITE)
font.drawbg(h, 10, 50, "readable", gfx.WHITE, gfx.BLUE)   -- 自带底色
local w  = font.width(h, "Hello")      -- 像素宽（比例字体按字形推进）
local lh = font.height(h)              -- 行高：16 px 时 prop 21 / mono 19

font.draw(font.mono(32), 20, 100, "12:34:56", gfx.CYAN)   -- 32 px：行高 38，格宽 19
font.draw(font.big(),    28,  73, "12:34:56", gfx.CYAN)   -- 56 px：行高 66，格宽 33
```

| 函数 | 说明 |
|---|---|
| `font.prop([size]) -> handle` | 比例字体句柄（Montserrat） |
| `font.mono([size]) -> handle` | 等宽字体句柄（Cascadia Code Light） |
| `font.cascadia([size]) -> handle` | 同 `font.mono`，只是把字体名字写出来 |
| `font.big() -> handle` | 钟表数字面（56 px 单档，无 size 参数，见下） |
| `font.height(handle) -> n` | 行高（含上下留白）：prop 16/24/32 px → 21/31/40，mono → 19/29/38，big → 66 |
| `font.width(handle, s) -> n` | 字符串像素宽。参考值：16 px prop `"Hello, World!"` = 101，mono = 130（13 格 × 10）；big `"12:34:56"` = 264 |
| `font.advance(handle, ch) -> n` | 单字符推进量（16 px：prop `"H"` 13，mono `"H"` 10；32 px mono 一律 19；big 一律 33） |
| `font.draw(handle, x, y, s, color)` | 画到**当前绘制目标**（`(x,y)` 是行左上角） |
| `font.drawbg(handle, x, y, s, fg, bg)` | 先用 `bg` 填文字框再画字 |

`handle` 是 userdata，`font.draw(handle, ...)` 与 `handle:height()` 两种写法都行
（句柄的 `__index` 指向库表）。`font.draw` 也受 `gfx.select()` 影响——想离屏合成
就 `gfx.select(g)` 再画。

**尺寸参数**：`size` 可以是任何数，**取最近的一档**（primetcc 引擎自己的规则）：
`font.prop(20)` 画 24 px，`font.mono(14)` 画 16 px，`font.prop(64)` 落到 32 px。
不传就是 16 px。`font.big()` 没有 size 参数：它是**单档**的字面。

**`font.big()` 是数字面，不是第三套正文字体**：它只有 14 个字形
（`'-' '.' '/' '0'-'9' ':'`），画别的字符**什么都不画**（不是画方块、也不是问号）。
它存在的理由是"大"和"清楚"的矛盾——把 32 px 的面放大 2 倍，抗锯齿边缘也一起放大，
每个 1 px 灰边变成 2 px 灰带，真机上明显发糊；所以这个面是**按 56 px 原生栅格化**的
（`port/plua_font_big_data.c`，`make font-big` 重新生成，`BIGSIZE=64` 可以换尺寸）。
`examples/clock.lua` 用它画时间，日期行仍走 5×8 点阵。

**没有的东西**：两族正文不支持非 ASCII（字形表是 0x20–0x7E）；没有斜体/粗体，没有
自动换行；`font.big()` 的字形集见上。想要 5×8 的点阵字（更省、更清楚）用 `gfx.text`。

---

## 4. `key`：键盘与触摸

按键和触摸通过**固件输入槽钩子**送达：先 `key.install()`，退出前 `key.remove()`。
钩子装着的时候系统仍然正常收键收触摸——**事件是只读观察的**，操作系统看到的输入和
没装钩子时逐字节相同。

实现是 primeLua 自己的（`port/plua_input.c`），**机制照搬 PureDOOM**：固件的
`get_event` 是阻塞的、归系统输入线程所有，所以只能"改 API 槽位 + 观察"，不能直接轮询。
DOOM 在这台机器上用同一个槽位读键盘和触摸一直好用（primeLua 此前两版自己发明的写法都在
真机上把输入弄死了），所以这一版是它的机制 + primeLua 自己的队列。

**默认就是装的**（`key.install()` 直接返回 `true`）；不想要的话，在应用目录放一个空文件
**`noinputhook`**，启动器会把策略字写成"拒绝"，此时 `key.install()` **返回 `false` 并且
不碰固件槽位**。

```lua
key.install()
gfx.init(); gfx.select(false)
while true do
  local k = key.getkey(500)              -- 最多等 500 ms
  if not k then break end                -- nil = 没按键
  if k == key.ESC then break end
  gfx.fillrect(4, 4, 40, 20, gfx.CYAN)
  gfx.text(8, 8, tostring(k), gfx.BLACK)
  sys.sleep(20)
end
key.remove()

-- 触摸：和键盘走同一条队列
local ev = key.event()                   -- {type="touch", phase=1, x=, y=, dx=, dy=}
if ev and ev.type == "touch" then
  local down, x, y = key.touch()         -- 现在手指还在不在、在哪
end
```

**运行时怎么中断**：程序跑着的时候，启动器一边流式打印输出、一边看键盘，按 **ESC**
（或 **ON**）就相当于让脚本 `error("interrupted")`（`pcall` 接得住，见 README §7.5）——
这是 primeLua 的 KeyboardInterrupt，脚本正卡在 `sys.sleep`/`key.getkey` 里也照样响应
（最坏 20 ms）。

**哪些脚本能被中断**：打印、画图、sleep、等键的循环**默认**就能被中断（`print`/`io.write`
和每个 `gfx.*` 绑定自己查中断字，不要钱）；**纯计算循环**（`fib` 这种一次 I/O 都没有的）
默认打断不了，会跑到底——要连它也打断，在应用目录放一个空文件 **`interrupt`**，代价是
解释器整体慢一倍（Lua 调试钩子的固有开销，实测 +108% 指令数）。两个键的来路不同：ESC 是启动器自己读到的（位掩码，4），**ON 是系统的
中断键** —— 固件会给正在跑的 Python 程序抛 `KeyboardInterrupt`，启动器接住并翻译成
同一个中断，所以从脚本看两者一模一样。

| 函数 | 说明 |
|---|---|
| `key.install() -> bool` | 装钩子（补固件 get_event 槽）。重复调用是安全的 |
| `key.remove()` | 拆钩子并**还原固件槽**，然后**回读验证**（结果进 `key.hookstats()` 的 `restore_verified`/`restore_failed`）。程序不调它也不会留后患：**解释器自己**在每次运行结束时（`plua_run_finish`）先还一次，启动器随后再核对一遍，两边都是回读验证的 |
| `key.hooked() -> bool` | 钩子是否在位（armed **且**槽位里确实是我们的补丁） |
| `key.hookstats() -> table` | **排错用**：输入路径的 20 个计数器——`armed`、`slot`、`calls`、`dropped_unarmed`、`bad_type`、`key_press/key_release`、`touch_press/touch_moves/touch_jitter/touch_release`、`touch_frames`、`touching`、`touch_x/touch_y`、`queue_full`、`restore_writes`、`restore_verified`、`restore_failed`、`installs`。真机上输入失灵时，它直接说明哪一层断了、上次退出时槽位恢复有没有被回读验证 |
| `key.getkey([wait_ms]) -> code｜nil` | 取一个**按下**事件（跳过释放）。**设备键码**，无键时 `nil`（不是 0）。`wait_ms` 内没等到也返回 `nil` |
| `key.gettouch([wait_ms]) -> x, y｜nil` | 取触摸按下或移动，坐标与 `gfx` 同系（0,0 左上） |
| `key.touch() -> down, x, y｜down=false` | **当前活触点**（DOOM 的 `g_is_touching` / `g_last_touch_x/y`）：有没有手指按着，最近一次**被接受**的位置 |
| `key.event() -> table｜nil` | 非阻塞取一个事件：`{type="key", code=n, down=bool}` 或 `{type="touch", phase=1/2/8, x=, y=, dx=, dy=}`。**phase 是固件自己的动作号**：1 按下、2 移动、8 抬起；移动的 `dx/dy` 是相对上一次被接受位置的位移，x、y 都只差 1 px 的移动会被当成抖动丢掉（计数在 `hookstats().touch_jitter`） |
| `key.sleep(ms)` | 等同 `sys.sleep(ms)`（`key` 里的同名便捷函数） |

**常量**：`ESC LEFT UP RIGHT DOWN BACKSPACE ENTER SPACE ON SHIFT APPS SYMB PLOT
NUM VIEW CAS MENU HELP ALPHA PLUSMINUS X2 Q`——值是**设备键码**，见
[第 10 节](#10-键码表)。**不要**把 PPL `GETKEY` 的编号拿来比较（那是另一张表，
混用会出现"按右键却退出程序"这类怪事）。

---

## 5. `sys`：系统

| 函数 | 说明 |
|---|---|
| `sys.version() -> "1.0"` | 解释器 API 版本。脚本可以据此判断某个新绑定是否存在 |
| `sys.time() -> n / nil` | **固件实时钟**的 Unix 秒（sdklib `GetSysTime`，svc 0x100A5）。固件不答时返回 **nil**（不是 0）——所以它能区分"真钟"和"只有启动器读的那一次"。`examples/clock.lua` 就是这么判断的 |
| `sys.clock() -> n` | 解释器起来以后过了多少秒（固件计数器现场标定，见 README §4.4）。没有时钟源时 0 |
| `sys.log(text)` | 把一行追加到**持久日志** `plua.log`（C 侧同一条通道，写穿不复位就丢）。真机排错用：在危险调用**之前**写标记，复位后日志最后一行就是凶手。**每次调用会重写整个文件，别放循环里** |
| `sys.sleep(ms)` | 让出 `ms` 毫秒（固件 `os_sleep`）；期间系统输入循环照常跑，钩子能收到键 |
| `sys.memory() -> n` | 固件堆"最大可分配块"的**二分探测**（反复 malloc/free 到 512 KB）。单位字节，粗略。**不要放帧循环**：这类大块 alloc/free 探针在真机上曾把机器弄复位 |
| `sys.maxalloc() -> n` | 同上的别名（最大单块） |

`os.time()`/`os.date()` 不需要 `sys.time()`：它们自己先问固件、再退回启动器读的
epoch；`sys.time()` 存在的意义是**告诉你现在走的是哪条路**：

```lua
if sys.time() then
  print("固件时钟：", os.date("%Y-%m-%d %H:%M:%S"))
else
  print("没有固件时钟，os.time() =", os.time())   -- 0 = 1970-01-01，不编
end
print("这次运行已经过了", sys.clock(), "秒")
```

---

## 6. `rand`：随机与噪声

PCG32（全周期、统计上过得去）+ 无状态值噪声。没调 `rand.seed()` 时用固定种子，
所以默认**可复现**。

| 函数 | 说明 |
|---|---|
| `rand.seed(n)` | 设种子（整数） |
| `rand.u32() -> n` | `[0, 2^32)` |
| `rand.range(lo, hi) -> n` | 含两端的整数 `[lo, hi]` |
| `rand.noise(x, y [, seed]) -> n` | 由整数坐标得到 `[0,1)` 的平滑噪声（同样输入同样输出，适合地形/云/星空） |

---

## 7. `codec`：编码与校验

| 函数 | 说明 |
|---|---|
| `codec.crc32(s) -> n` | CRC-32（`codec.crc32("123456789") == 0xCBF43926`） |
| `codec.adler32(s) -> n` | Adler-32 |
| `codec.hex(s) -> s` | 转十六进制字符串，**小写**（`codec.hex("Hi") == "4869"`） |
| `codec.unhex(s) -> s` | 反向；坏输入抛错 |
| `codec.base64(s) -> s` | 标准 base64 编码 |
| `codec.unbase64(s) -> s` | 反向；坏输入抛错 |

二进制安全：都接受/返回任意字节的 Lua 字符串（含 `\0`）。

---

## 8. `fx`：Q16.16 定点

没有 FPU 时定点比 `double` 快得多（游戏/像素运算常用）。值是 Q16.16 的整数：
`1.0` 存成 `65536`。**注意返回类型**：大多数函数返回 Q16.16 整数，但
`todouble` 返回普通 number，`trunc`/`round`/`int` 返回普通整数（整数值）。

| 函数 | 说明 |
|---|---|
| `fx.fromdouble(x) -> q` | number → Q16.16（`fx.fromdouble(1.5) == 98304`） |
| `fx.todouble(q) -> x` | Q16.16 → number（`fx.todouble(fx.fromdouble(1.5)) == 1.5`） |
| `fx.int(n) -> q` | 整数 → Q16.16（`fx.int(7) == 458752`） |
| `fx.trunc(q) -> n` / `fx.round(q) -> n` | 取整/四舍五入，返回**普通整数** |
| `fx.neg(q) fx.abs(q) fx.sqrt(q) fx.sin(q) fx.cos(q) fx.tan(q)` | 一元运算，返回 Q16.16 |
| `fx.add(a,b) fx.sub(a,b) fx.mul(a,b) fx.div(a,b) fx.atan2(y,x) fx.lerp(a,b,t)` | 二元/三元，返回 Q16.16 |

精度约 1/65536 ≈ 1.5e-5；范围 ±32768。

---

## 9. `dd`：double-double（31 位十进制）

**独立的新库**：Lua 的 `math` 与数字类型一行没动（差分测试要求与桌面 Lua 逐位一致），
高精度放在自己的命名空间。一个 `dd` 是两个 double（hi+lo，约 106 位 ≈ 31–32 位十进制）。

```lua
local pi = dd.pi()
print(pi)                              --> 3.14159265358979323846264338327951e+00
print(dd(2) * pi)                      --> 6.28318530717958647692528676655901e+00
print(dd.sqrt(dd(2)), dd.sin(dd(1)))   -- 都是 33 位
print(dd.tostring(pi, 20))             --> 3.1415926535897932385e+00
print(dd("3.14159265358979323846264338327950288"))   -- 从长字符串解析
print(dd(9007199254740993) - dd(9007199254740992))   --> 1（double 会给 0）
print(dd.pi():sqrt(), dd(1) < dd(2))   -- 方法/比较
local hi, lo = dd.parts(dd(1)/dd(3))   -- 精确的两个 double
```

### 构造与转换

| 函数 | 说明 |
|---|---|
| `dd(x [, lo])` | `dd(2)`（整数，**64 位精确**）、`dd(1.5)`（double）、`dd("0.1")`（十进制度数不限，按十进制值而不是 double 值解析）；`dd(hi, lo)` 直接给两个字 |
| `dd.new(hi [, lo]) -> dd` | 同上，函数写法 |
| `dd.parts(x) -> hi, lo` | 精确的两个 double（不四舍五入） |
| `dd.tonumber(x) -> n` | 最近的双精度数 |
| `dd.tostring(x [, 位数]) -> s` | 十进制字符串，默认 33 位有效数字（1–33） |
| `dd.isnan(x) / dd.isinf(x) -> bool` | 判定 |
| `dd.pi() dd.e() dd.ln2() dd.ln10()` | 常数，正确舍入到 dd |

### 运算符与函数

`+ - * / ^ == ~= < <=`（两侧都接受 dd 或普通数字；比较对 NaN 一律 false）、
一元 `-`、`tostring(x)`。

`dd.abs sqrt exp log log10 sin cos tan asin acos atan atan2 pow fmod hypot
floor ceil neg`，以及函数写法的 `dd.add/sub/mul/div`。

`dd.exp(x)` 在 |x| > 709.78 溢出为 inf；`dd.log(0)` = -inf；负数开方返回 nan。

### 精度（`make test-dd`，mpmath 60 位真值，dd-ulp = 2^-106）

sin 4.1 · cos 2.2 · tan 3.3 · asin 4.6 · acos 2.4 · atan 1.5 · atan2 4.3 ·
**exp 45.6**（|x|>300 时约 30 位）· log 2.7 · log10 3.7 · sqrt 1.3 · pow 10.0 ·
hypot 1.2；π/e/ln2/ln10 精确。解析 31 位以上输入约 10 dd-ulp，打印最后一位可能差 1–2。

**已知取舍**：`dd("0.1") + dd("0.2") == dd("0.3")` 是 `false`——`dd("0.1")` 是*十进制*
0.1（比 double 的 0.1 差 5.55e-18，这正是它的意义），两个值各带约 1e-32 舍入，和比
0.3 低约 2e-32（打印出来是 `2.99999999999999999999999999999998e-01`）。参数归约是
三字 Cody-Waite，所以 sin/cos 在 |x| 到 1e6 仍保持精度；算法、实测表与实现坑见
`port/plua_dd_NOTES.md`。

---

## 10. 键码表

**项目里有两张不同的表**——Lua 侧用上面那张，Python 启动器用下面那张：

| 键 | `key.*` 常量 / `key.getkey()` 返回（**设备键码**） | PPL `GETKEY` / `hpprime.keyboard()` 位号 |
|---|---|---|
| esc | `0x01` (1) | 4 |
| ← 左 | `0x02` (2) | 7 |
| ↑ 上 | `0x03` (3) | 2 |
| → 右 | `0x04` (4) | 8 |
| ↓ 下 | `0x05` (5) | 12 |
| 退格 | `0x0C` (12) | 19 |
| 回车 | `0x0D` (13) | 30 |
| 空格 | `0x20` (32) | 49 |
| ON | `0x83` | — |
| APPS / SYMB / PLOT / NUM / VIEW / CAS / MENU | `0xB1 / 0x91 / 0xB2 / 0xB3 / 0xB4 / 0xB5 / 0x93` | 0 / 1 / 6 / 11 / 9 / 10 / 13 |
| HELP / ALPHA / ± / x² | `0x95 / 0xB6 / 0x4D / 0x4C` | 3 / 36 / — / — |
| 7/Q 键 | `0x51`（alpha 模式下是 `q`） | 32 |

设备键码来自 primetcc 的 `rt/hp_input.h`（对着计算器扫描码矩阵核对过），`key.*`
直接用那套宏注册。**踩过的坑**：曾经把 PPL `GETKEY` 编号当设备键码注册，于是按右键
却退出程序（右键 `0x04` 正好是 GETKEY 的 esc=4）、按左键变成向上。

---

## 11. 颜色与尺寸

- 颜色是**整数**（`lua_Integer`）`0xAARRGGBB`，所以可以直接比较/表键：
  `gfx.BLACK == 0xFF000000`、`gfx.RED == 0xFFFF0000`。`gfx.rgb(r,g,b)` 补上不透明 alpha。
- 预置：`gfx.BLACK WHITE RED GREEN BLUE YELLOW CYAN MAGENTA ORANGE GRAY`。
- 屏幕：`gfx.WIDTH` = 320，`gfx.HEIGHT` = 240（`gfx.width()/height()` 同值）。
- 内置 5×8 字体：字符格 5×8，步进 6 px，一行 53 字符。
- `font` 两族正文各 16 / 24 / 32 px：行高 prop 21/31/40、mono 19/29/38，等宽格
  16 px 是 10、32 px 是 19；`font.big()` 是 56 px 单档，行高 66、格宽 33
  （`font.height()`/`font.advance()` 取准确值）。

---

## 12. 例子

**画一个会动的方块并响应按键**（`examples/gfxdemo.lua` 的骨架）：

```lua
local gfx, key, sys = gfx, key, sys
gfx.init()                   -- 目标就是屏幕，无需 blit
gfx.select(false)
gfx.clear(gfx.BLACK)
key.install()
local x, y = 100, 80
for frame = 1, 400 do
  for _ = 1, 8 do
    local k = key.getkey()
    if k == key.ESC then frame = 400 break end
    if k == key.LEFT  then x = x - 4 end
    if k == key.RIGHT then x = x + 4 end
    if k == key.UP    then y = y - 4 end
    if k == key.DOWN  then y = y + 4 end
  end
  gfx.clear(gfx.BLACK)
  gfx.fillrect(x, y, 40, 40, gfx.CYAN)
  sys.sleep(20)
end
key.remove()
```

**用真字体画一段状态行**：

```lua
local h = font.prop()
gfx.clear(gfx.BLACK)
font.drawbg(h, 6, 6, "primeLua " .. sys.version(), gfx.WHITE, gfx.BLUE)
font.draw(h, 6, 34, "free: " .. (sys.memory() // 1024) .. " KB", gfx.GRAY)
```

**拿 32 px 的等宽字体画个大数字（时钟/计数器）**——等宽格保证数字不会左右跳：

```lua
local big = font.mono(32)                 -- 与 font.cascadia(32) 同一个字面
local cell, lh = font.advance(big, "0"), font.height(big)   -- 19, 38
gfx.textbg(2, 2, os.date("%Y-%m-%d %H:%M:%S"), gfx.WHITE, gfx.BLACK)
font.draw(big, math.floor((gfx.WIDTH - 8 * cell) / 2), 100,
          os.date("%H:%M:%S"), gfx.CYAN)
```

**31 位精度算 π 与 e**：

```lua
print(dd.pi())                                  -- 33 位
print(dd.exp(dd(1)))                            -- e
print(dd.tostring(dd.pi() * dd(2), 20))         -- 只要 20 位
print(dd.tostring(dd.log(dd(10)), 32))          -- 2.3025850929940456840179914546844e+00
                                                -- （与 dd.ln10() 前 31 位一致，但不逐位相等：
                                                --   log 是算出来的，ln10 是正确舍入的常数）
```

**离屏合成（GROB）**：

```lua
local bg  = gfx.newgrob(320, 240)
local spr = gfx.newgrob(16, 16)
gfx.select(bg)  gfx.clear(gfx.BLUE)
gfx.select(spr) gfx.clear(gfx.RED)  gfx.fillcircle(8, 8, 7, gfx.YELLOW)
gfx.select(bg)  gfx.blit(spr, 40, 60)           -- 拷到当前目标（bg）
gfx.blit(bg)                                    -- 上屏
gfx.freegrob(spr); gfx.freegrob(bg)
```

**32 位有符号和/校验**：

```lua
print(string.format("%08X", codec.crc32("123456789")))   -- CBF43926
print(codec.unbase64(codec.base64("hi")) == "hi")        -- true
```

---

## 13. 已知限制

| 项 | 说明 |
|---|---|
| 输出只有 32 KB | 环形缓冲所限；`for i=1,1e6 do print(i) end` 只看得到尾部（写得太快时最前面可能是半行——环形缓冲存的是字节） |
| 运行时 **ESC / ON** 会**中断**脚本 | 启动器在流式显示时看着键盘：按 ESC（它自己读）或 ON（系统中断键，它接住 `KeyboardInterrupt`）都相当于让脚本 `error("interrupted")`（`pcall` 能接住），卡在阻塞调用里也能响应。纯计算循环要先放 `interrupt` 标记文件（见上） |
| `os.clock()` 不是 CPU 时间 | 它是"解释器起来以后过了多少秒"；没有固件时钟时返回 0 |
| 时间源 | 固件实时钟 → 启动器读的 PPL epoch → 0（1970）。都不做假，用 `sys.time()` 分辨 |
| 没有真正的 REPL | 逐行求值需要输入源；启动器菜单里的 `= <lua>` 是单行版 |
| 中断 | 程序跑起来就不能从键盘打断（不支持信号）；所以示例都带帧数/迭代上限 |
| 图形/键盘 | `gfx`/`key`/`font` 可用；运行期间 Python 侧阻塞但屏幕照常刷新 |
| 字体 | 两族正文（Montserrat 比例 / Cascadia 等宽）各 16 / 24 / 32 px，ASCII 0x20–0x7E；`font.big()` 是单档 56 px 的**数字面**（14 个字形）。没有斜体/粗体 |
| 精度 | `math` 与 glibc 同档（≤1.31 ulp）；更高精度用 `dd`（≈31 位，慢一个量级） |
| 内存 | 解释器镜像 522 KB（strip 后；装载 487 KB，其中字模 ~43 KB）；`gfx.select()` 的后缓冲 307 KB，用完记得 `freebackbuf()` |
| `require` | 只能加载 `.lua`；路径固定为应用目录与 `C:\DATA`；无 C 模块（`ti` 库这一版没有，见 `../_attic/primeLua-ti/`） |

---

*文献索引：`README.md`（移植与测试全貌）、`port/plua_dd_NOTES.md`（dd 的算法与实测）、
`../HP_Prime_GETKEY.md`（两张键码表）、`sys.log()` 写出的 `plua.log` 与启动器的
`plua_launcher.log`（真机排错）。*
