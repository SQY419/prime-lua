# primeLua — 把 Lua 5.4 移植到 HP Prime G1

> 在计算器上跑的是**官方 Lua 5.4.7 解释器本体**（PUC-Rio 源码，交叉编译成
> ARMv5TE 的 `lua.elf`），不是重写品、不是子集、不是"类 Lua"。
> 与桌面 `lua` 的差异只有那些**物理上无法相同**的部分（没有进程、没有环境变量、
> 输出走环形缓冲），并且全部列在 §8。

---

## 1. 交付物与现状

| 项目 | 内容 |
|---|---|
| 解释器 | `primeLua.hpappdir/lua.elf` — **API 版本 1.0**，strip 后 **488 KB**（500,292 B），ELF32 DYN (PIE)，入口 `plua_entry`，857 条 `R_ARM_RELATIVE` 重定位 |
| 镜像尺寸 | text 388,854 B / data 37,852 B / bss 29,856 B（bss 里 32 KB 那块是输出环形缓冲）；两个 PT_LOAD 共装载 **456 KB**（只读段 380 KB：解释器与引擎 + 几乎全是字模的 .rodata；读写段 37 KB）。`make` 每次会打印当前数字 |
| 运行方式 | 解释器跑在**固件线程**上（`svc 0x10000`，PureDOOM 同款），入口立刻把运行状态交回启动器 → 输出**边跑边显示**、按 **ESC/ON 中断**（§7.5） |
| 字体 | **Montserrat（比例）/ Cascadia Code Light（等宽），各 16 / 24 / 32 px**，4-bit 抗锯齿；两族字模共 ~43 KB（`port/plua_font_montserrat_data.c`、`port/plua_font_cascadia_data.c`）。另有钟表专用的大数字面 **`font.big()`：Cascadia 原生 56 px**（`port/plua_font_big_data.c`，只含 `'-' '.' '/' '0'-'9' ':'`，6.6 KB） |
| 本机时钟 | `os.time()` / `os.date()` / `os.clock()` 走**固件实时钟**（svc 0x100A5，真机实测，见 §4.4）；固件不答时退回启动器从 PPL `Date`/`Time` 读的 epoch，两者都没有就老老实实返回 0（1970），不编时间 |
| 计算器端启动器 | `primeLua.hpappdir/main.py` — **被刻意做小**（156 KB → 87 KB：注释与 docstring 都搬到 `main_notes.md`，因为固件的 Python 把整个源码编进内存，实测 156 KB 源码要 6 MB 堆）。屏幕上是词典风格的列表（一 GROB、每帧一次 BLIT、无 footer），程序运行期间**轮询环形缓冲、边跑边彩色打印**，**ESC 中断（ON 也接住）**，按任意键回列表 |
| 示例脚本 | `hello.lua` / `sieve.lua` / `fib.lua` / `mandel.lua`（**图形版**曼德博集）/ `gfxdemo.lua`（图形+键盘）/ `clock.lua`（**走真时间的钟**，`font.big()` 56 px 数字）/ `keydiag.lua` / `keyprobe.lua`（输入钩子逐级探针）/ `hwcheck.lua`（真机排错梯子）（随部署目录一起拷） |
| 目标机型 | HP Prime G1：ARM926EJ-S (ARMv5TEJ)、soft-float、无 FPU |
| Lua 版本 | 5.4.7（含 `LUA_COMPAT_5_3`，与上游 Makefile 的 `make posix` 完全一致） |
| 数值类型 | 默认 double + 64 位整数（`LUA_32BITS` 未启用） |
| 标准库 | 全部：base / coroutine / table / string / math / io / os / utf8 / debug / package |

**不在这份交付里**：`ti` 库（TI-Nspire 兼容层）——先不做，源码连同示例一起搬到了
树外的 `../_attic/primeLua-ti/`，`make deploy` 也不再往部署目录拷 `ti.lua`。

**已验证能跑**（差分测试逐字节对齐桌面 Lua，见 §5）：
字符串与模式匹配、表与元表（含 `__close`/`__gc`/弱表）、协程（含跨协程 `pcall` 与
to-be-closed）、闭包与上值、可变参数、尾调用、goto、错误处理（`pcall`/`xpcall`/
`error` 层级）、整数/浮点子类型与位运算、`string.format` 全部转换符、
`string.pack/unpack`、`load`/`loadfile`/`dofile`/`require`/`string.dump` 字节码、
`math.*`（openlibm ~1ulp）、`os.date/os.time/os.difftime`、
`io.open/read/write/seek/lines`（真实落到固件文件 API）、`utf8.*`、`debug.*`。

---

## 2. 它是怎么跑起来的

```
计算器（MicroPython 应用）
  └─ import main                       ← primeLua.hpappdir/main.py
       ├─ 固件文件 API 列出 C:\DATA\primeLua.hpappdir\*.lua（find_first/next）
       └─ 选中脚本后：
            ├─ shellcode ELF loader（DOOM / primetcc 同款，已上真机验证）
            │    · malloc 一块 8 字节对齐的镜像内存，加载 PT_LOAD 段
            │    · 清零 .bss 尾巴、应用 R_ARM_RELATIVE 重定位
            ├─ 构造 struct plua_config {magic, argc, argv, epoch, heap}
            └─ dbg.call(plua_entry, &config, 0)        ← r0 = 配置指针
                 │
lua.elf          ▼
  plua_entry (port/plua_svc.s)
    · 保存 r4-r11/ip/lr（loader 的 shellcode 信任 AAPCS）
    · SP 对齐到 8（ARM926 上未对齐的 LDRD/STRD 会触发 Data Abort → 整机复位）
    · 从固件堆 malloc 程序栈（首选 128 KB，失败逐级减半；Lua 的递归下降解析器需要）
    · plua_begin(r0)：登记 config、初始化控制台
    · plua_main() → lua.c 的 main()（原封不动，只把符号名改成
      plua_lua_standalone_main）
    · 结束后写 "RT_RET:<n>" 到环形缓冲、归还栈、恢复所有寄存器、返回
         │
         ▼
      Lua 5.4.7 VM ── print/io.write ──► PLUARING 环形缓冲（32 KB）
                                          ▲
计算器端 main.py ── 轮询 ─────────────────┘  边跑边打印（流式，§7.5）
```

**1.0 起，程序跑在固件线程上**：`plua_entry` 用固件的 `svc 0x10000` 起一个线程
（`sys_create_thread(plua_thread_entry, &config, 512 KB)`，就是 PureDOOM 的 `main()`
起游戏线程那一句），把运行状态结构体的地址交回启动器后**立刻返回**。于是启动器在程序
还在跑的时候就拿回了控制权，可以一边读环形缓冲一边打印（§7.5），也可以在用户按 ESC
时把运行状态里的 `abort` 字置 1，让解释器自己的调试钩子把脚本 error 掉。

```
lua.elf          ▼
  plua_entry (port/plua_svc.s)
    · 保存 r4-r11/ip/lr（loader 的 shellcode 信任 AAPCS）
    · SP 对齐到 8（ARM926 上未对齐的 LDRD/STRD 会触发 Data Abort → 整机复位）
    · plua_run_thread_start(&config)（port/plua_run.c）
         ├─ 填运行状态 {state, mode, ring, ring_start, ...}（magic "PLUARUN"）
         ├─ svc 0x10000 起线程：r0=线程体、r1=0、r2=512 KB 栈（逐字照抄 DOOM）
         ├─ svc 0x10003 设优先级：r0=create_thread 返回的句柄、r1=60（次序错了整机复位）
         └─ 返回运行状态地址 → r0 → 启动器
              │
   固件线程   ▼  plua_thread_entry → plua_thread_main(config)
    · plua_begin(config)：登记 config、初始化控制台、写 plua.log
    · plua_main() → lua.c 的 main()（原封不动，只把符号名改成
      plua_lua_standalone_main）
    · 结束后写 "RT_RET:<n>" 到环形缓冲、把 state 置 2（DONE）、线程返回
```

* **argv 从哪来**：计算器没有 `argv`。启动器把 `{"lua", "C:\\DATA\\...\\脚本.lua"}`
  写进配置块，`plua_main` 取出后原样交给 `lua.c` 的参数解析 —— 所以 `-e`、`-i`、
  脚本参数等行为与桌面一致。
* **输出为什么是环形缓冲**：C 侧把 stdout/stderr 写进镜像内的 32 KB 环形缓冲
  （magic `PLUARING`），启动器在里面追着读。环形缓冲是**线程之间的信箱**：解释器
  只管往里写（不阻塞、不等待谁来看），启动器按 `count` 追进度；真机上"没人在看"
  时（比如从终端直接跑）它也只是覆盖旧字节而已。代价是只保留最后 32 KB（§8）。
* **为什么不用 primetcc 的 PRIMELOG**：primetcc 的环形缓冲是 `hp_rt.c` 的 static
  变量，写它只能经由它自己的 `printf`。primeLua 换成了自己的引擎（§4），
  于是也自带一块 ring，并把 magic 换成 `PLUARING`，避免同一镜像里两个环混淆。
* **时间从哪来**：`os.time()`/`os.date()`/`os.clock()` 先问**固件实时钟**
  （svc 0x100A5，`port/plua_time.c`），失败才退回启动器每次运行前从 PPL
  `Date`/`Time` 读一次、写进配置块 `epoch` 的那个值；两个源都没有就返回 0
  （1970-01-01）而不是编一个 —— 完整实测见 §4.4。

### 内存账

| 项 | 大小 |
|---|---|
| 镜像（loader malloc，8 对齐） | ~455 KB（两个 PT_LOAD 的文件字节：390,640 + 70,220） |
| 其中：代码 + 字模（只读段） | ~381 KB（.text 301 KB 是解释器+字体引擎，.rodata 82 KB 几乎全是两族字模） |
| 程序栈 | **线程栈 512 KB**，由固件在 `svc 0x10000` 时分配、线程结束时收回（DOOM 要的也是 512 KB）。旧的"自己 malloc 128 KB 程序栈"只在 `nothread` 的同步模式里还用（失败就 64/32 KB，最后退到 24 KB 静态缓冲） |
| Lua 堆（固件堆，按需） | 脚本决定；测试套件实测峰值约 400 KB |
| 输出环形缓冲（镜像 .data 内） | 32 KB |

固件堆在模拟器里的峰值（含镜像+栈+脚本数据）见 §5 的 insn/heap 统计。
真机上启动器会先把 `heap_free` 写进日志：这一版镜像比以前大（多了 24/32 px 两档
字模），如果日志里 `heap_free` 只剩几百 KB，就别再往脚本里塞大表。

---

## 3. primeLua 自己实现的平台层

Lua 不依赖 libc，只依赖一组 C 函数。`port/` 下的文件提供它们；**移植的难点全在这里，
Lua 本体一行没改**。

| 文件 | 作用 | 关键点 |
|---|---|---|
| `plua_svc.s` | 固件 SVC 包装 + `setjmp/longjmp` + 入口 `plua_entry` 与线程入口 `plua_thread_entry` | SVC 约定 `push{r0};push{lr};svc N`；`longjmp` 是 Lua 全部错误机制的基石（`ldo.c` 的 `LUAI_THROW`）；入口里 `bic sp,sp,#7` 与"压两个字"的栈对齐都是真机 Data Abort 换来的 |
| `plua_main.c` | 入口粘合：config → argc/argv、`exit()` 展开、`RT_RET` | `exit()` 没有可退之处，longjmp 回这里，让入口还能给启动器返回状态 |
| `plua_run.c` | **运行方式**：svc 0x10000 起固件线程跑解释器、运行状态（magic `PLUARUN`）、中断（abort）字与调试钩子检查 | 这是**流式输出**的全部秘密（§7.5）：入口立刻返回，启动器边跑边读；线程栈由固件给（512 KB），所以同步模式里那个 malloc 的程序栈只在 `nothread` 下才用 |
| `plua_format.c` | **自己的 C99 printf 引擎** | 见 §4.1。`%.14g` 是 Lua 打印每个数字用的格式 |
| `plua_strtod.c` | **自己的正确舍入 strtod** | 见 §4.2。Lua 词法分析器用它解析每个数字字面量 |
| `plua_file.c` | FILE 层：stdout/stderr → 环形缓冲，文件 → 固件宽字符 API | 必须实现 `ungetc` 与 `freopen`：`luaL_loadfilex` 靠它们嗅探 `#`/BOM/字节码 |
| `plua_libc.c` | `realloc`（固件堆，与 primetcc 分配器头布局互通）、`exit/abort`、`errno`、`locale`、`ctype`、`strerror`（glibc 同字面）、`strtoul` | 固件堆只保证 4 字节对齐，而 ARM926 上未对齐 LDRD/STRD 会复位 → realloc 里要做对齐搬迁 |
| `plua_math.c` | 标准名 → openlibm 的 `hp_*` | 复用 primetcc 预编译的 `rt_math.o`（1 ulp），另补 openlibm 缺的 `log2`（`port/openlibm-extra/`）与 `copysign` |
| `plua_time.c` | `gmtime/localtime/mktime/strftime/difftime/clock` | `%c/%x/%X/%e` 对齐 glibc 的 C locale；**strftime 必须容忍 `fmt == s`**（Lua 的 `os.date` 就是这么调的） |
| `port/include/` | 遮蔽 newlib 的头文件（stdio/stdlib/string/math/errno/locale/time/ctype/setjmp/signal/assert） | 只留 GCC 自带的 `stddef/stdarg/stdint/float/limits` |

**从 primetcc 复用的部分**（编译成 primeLua 自己的目标文件，不改 primetcc 源码）：
固件堆 `malloc/calloc/free`、`mem*/str*` 核心、`qsort/bsearch`、openlibm 数学库。
`plua_format.c` / `plua_strtod.c` 顶掉了运行时里对应的旧实现（Makefile 里用 `-D` 改名
让它们不参与链接），理由见 §4。

---

## 4. 两处保真度攻坚

移植"能跑"不难，难的是**每个数字都和桌面 Lua 一模一样**。这两块是 primeLua 花最大
力气的地方，也是它区别于"能打印 hello world 的玩具"的地方。

### 4.1 printf 引擎（`port/plua_format.c`）

primetcc 运行时的引擎是为 demo 写的，对 Lua 有三个致命问题：

1. `%g` **忽略精度**，直接输出最短往返文本 —— 而 Lua 打印数字用 `%.14g`，
   于是 `print(1/3)` 会打出 `0.333333333333333`（15 位）而不是 `0.33333333333333`；
2. `%f` 只支持 15 位小数、进位一律远离零（glibc 是 round-half-even）；
3. **≥1e19 直接打印字面量 `>1e19`** —— `print(1e20)` 输出 `>1e19`。

所以 primeLua 自己写了引擎。数值核心是**精确**的而不是近似的：任何有限 double 都是
有限十进制小数（`x = m·2^e`，`e<0` 时恰好是 `(m·5^-e)/10^-e`），于是用大整数算出
`m·5^-e` 就得到该值的**全部精确十进制位**（最多约 1080 位），再按任何精度做十进制
舍入 —— 没有二次舍入、没有粘滞位猜测，平局按 glibc 的 round-half-even。

支持：标志 `- + 空格 # 0`、宽度与精度（含 `*`）、长度修饰 `h hh l l z t j`、
转换符 `d i u o x X c s p f F e E g G a A %`，以及真正的 snprintf 截断语义
（返回完整长度、缓冲一定 NUL 结尾）。

**验证**：`make test-format`，与 glibc 逐字节比对 **162,966 条**
（含 4000 个随机 double × 40 种格式、边界值/次正规数/NaN/inf、64 位整数边界、
截断、`*` 宽度精度）。结果为 **0 失败**，另有 2 条**已记录的 glibc 偏差**
（C11 规定 `%#g` 保留尾零，glibc 在进位跨十进位时会把它们去掉；primeLua 按标准走）。

### 4.2 strtod（`port/plua_strtod.c`）

Lua 的**每一个数字字面量**都经过 `strtod`。primetcc 运行时那份是浮点累加
（`v = v*10 + d; v += d*frac`），实测：`1e-20` 偏大 8 ulp、`1e308` 偏大 1 ulp、
`"inf"` 返回 `1.0e300`、完全没有十六进制浮点（`0x1p4` 解析成 `nil`）。

primeLua 的实现同样走精确路径：数字串累积成精确大整数 `D`，值就是 `D·10^E`；
`E<0` 时保留精确有理数 `D / 10^j` 做一次二进制长除法，取至少 121 位商**并保留精确
余数**（余数就是粘滞位），按 round-half-even 组装 IEEE 位型；次正规数按次正规精度
舍入，下溢是渐进的。十六进制浮点直接按二进制处理，并实现 glibc 的 NaN payload 语义。

**验证**：`make test-strtod`，与 glibc 比对 **2,450 条**（固定难点集 + 1500 条随机
十进制 + 800 条随机十六进制 + endptr 位置 + 位型全等），**0 失败**。

### 4.3 stftime / 日期（`port/plua_time.c`）

`time_t` 用 **64 位**（否则 `os.time{year=2100}` 会静默回绕成 1903 年）。
`strftime` 有两个坑，都是实测踩出来的：

* **Lua 的 `os.date` 把格式串和输出缓冲用同一块内存**
  （`strftime(buff, SIZETIMEFMT, buff, stm)`）—— 边读边写会自毁格式串，
  `os.date("!%j %Y")` 会打出 `001 0371`。primeLua 先在本地缓冲里成形再拷出。
* `%a/%b/%h` 是**缩写**、`%A/%B` 是全名；`os.date()` 默认走 `%c`，
  所以 `%c/%x/%X/%e` 都对齐 glibc 的 C locale。

**验证**：`make test-time`，与 glibc 比对 **3,435 条**（20+ 种格式 × 含闰年/负时间戳
的 18 个时间点 + 2000–2001 逐日扫描 + mktime 往返 2000 次），**0 失败**。

### 4.4 本机时间：固件实时钟（svc 0x100A5）

以前 `os.time()` 只能拿到启动器开跑前读一次的 PPL `Date`/`Time`（跑起来就不走了），
`os.clock()` 恒为 0。现在两个都走**计算器自己的实时钟**。

**结论先行**：SDKLIB 的 `GetSysTime` 本体就是 `push {r0}; push {lr}; svc 0x100A5`
—— 一个**带一个参数**的函数。primetcc 当年用 r0 = 0 调它（等于往 NULL 写），整机复位，
于是这件事被记成了"设备没有时钟源"；**给它一个真缓冲就一切正常**。2025-09-25 在真机上
量到（`calc/timeprobe.py` 的 risky 档，原始日志 `calc/timeprobe.log`）：

```
0x100A5 returned r0 = 0x000007EA (2026)
buf words: 000907EA 00190005 001F0014 02A7003A 000128F5 ...
```

按小端 u16 解开就是 **2026 / 9 / dow 5（周五）/ 25 日 20:31:58**，比同一次运行里 PPL
报的 20:31:56 晚两秒（正好是 `wait(2)`），两秒后的第二次采样是 20:32:00 —— 字段顺序
里没有猜的成分：

| u16 下标 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8..9 |
|---|---|---|---|---|---|---|---|---|---|
| 含义 | 年 | 月 | 星期（0=周日） | 日 | 时 | 分 | 秒 | 未用/未知 | 亚秒计数器（u32） |

* **`os.clock()` 的单位不靠假设**：第一次看到整秒跳变时，用"计数器差 ÷ 秒差"现场标定
  ticks-per-second（那次两秒里走了 1825 个单位 ≈ 900/s，**不是**整数毫秒），之后一直
  用它；标定之前按毫秒算。
* **读不到就不编**：固件连续 6 次答非所问就停止再问（`PLUA_FW_FAIL_MAX`，免得一次坏
  时钟把每个 `os.time()` 都变成固件调用），退回启动器写入配置块的 PPL epoch；两个源
  都没有时 `os.time()` 返回 0（1970-01-01）、`os.clock()` 返回 0，而 `sys.time()`
  如实返回 nil —— `examples/clock.lua` 就是靠这个区分"真钟"和"没有钟"，并显示
  `--:--:--` 而不是假装 1970。
* 每次 `os.time()` 都真调一次固件服务（代价与 malloc 同量级）；**不能**用"缓冲区没变
  就复用上次结果"来缓存 —— 缓冲区只在被调用时才更新，那样缓存会让时钟停在第一秒。

**验证**：`make test-time` 里有一组用例直接喂 2025-09-25 真机那 10 个 u16（期望值用
Python 独立算过 `datetime(2026,9,25,20,31,58,tzinfo=utc).timestamp()`），另加
1970–2100 逐日扫描；模拟器侧 `tests/emu_time.py` 按**同一套布局**实现 svc 0x100A5，
所以 `make test-launcher` / `make test` 里脚本看到的 `os.date()` 是真实日期。

**想在另一台机器上重新量**：把 `calc/timeprobe.py` 拷到计算器的 `C:\DATA\`，在 Python
应用里 `import timeprobe; timeprobe.run()`（安全档：只查 utime 与 PPL）→
`timeprobe.run(risky=True)`（会真的调 svc 0x100A5，日志落 `C:\DATA\timeprobe.log`）。

---

## 5. 测试体系（三层，全部可复现）

这个项目最花时间的不是写代码，而是**让"能跑"变成"可证明地对"**。

### 第 1 层：主机差分（秒级）

| 目标 | 内容 | 结果 |
|---|---|---|
| `make test-format` | printf 引擎 vs glibc | 162,966 检查 / 0 失败 |
| `make test-strtod` | strtod vs glibc | 2,450 检查 / 0 失败 |
| `make test-time` | strftime/gmtime/mktime vs glibc **+ 真机时钟结构解码**（§4.4） | 3,435 检查 / 0 失败 |
| `make test-modes` | 固件文件模式翻译（`"w"`→`"wb+"` 等） | 15 检查 / 0 失败 |
| `make test-dd` | double-double vs mpmath 60 位 | 680 函数用例 + 10 字符串用例，全部通过 |

### 第 2 层：32 位 ARM + qemu-arm（`make test-arm`）

主机是 aarch64（`long` 64 位），和计算器（32 位 `long`、soft-float）不是一回事，
所以同样三套测试会用 `arm-linux-gnueabihf-gcc` 交叉编译、在 **qemu-arm**（独立、
工业级 ARM 实现）下再跑一遍。**这一层立刻抓出了两个只在 32 位下暴露的真问题**：
`%zd` 被当成无符号读（负数变天文数字）、`time_t` 32 位导致 2038 溢出。
`plua_time.c` 里的固件时钟走 `plua_svc.s` 的 `hp_svc_gettime()`（ARM 专属、主机上
不能执行），所以这一层和 `make test-time` 一样用 `PLUA_GETTIME_CALL` 这个接缝换成
测试自己的假固件调用 —— 日历代码照样被跑到，链接也不会因为缺 `hp_svc_gettime` 而断。

### 第 3 层：启动器测试（`make test-launcher`）

`tests/test_launcher.py` 直接跑 **`calc/main.py` 本身**：mock 掉 MicroPython 的
`uio`/`ustruct`/`hpprime`，调试接口直接接到 ARM 模拟器上，于是启动器的每一段
（列目录、固件 malloc、shellcode loader、配置块、调入口、**流式读环形缓冲**、
按键 UI 循环、**触摸手势**）都在主机上被真正执行 —— 7 个场景、85 项检查，约 30 秒
（`--scenario NAME` 可以只跑一个）。

它按"启动器看到的世界"来建模：调入口**立刻**返回（程序在固件线程上跑），
启动器每次 `os_sleep` 才让程序前进一格（`tests/emu_threads.py` 的 `SLICE`），
所以"边跑边打印"在这里是可断言的。

**关键保真点**：mock 的 `uio.FileIO` 像真机一样**拒绝 `C:\...` 绝对路径**。
这条规则抓到了两个只在真机上才会发作的 bug：

| bug | 症状 |
|---|---|
| 把绝对路径交给了 `uio.FileIO` | `lua.elf` 读不到 → 闪一下回菜单（**用户实测到的**） |
| `RingReader` 起始偏移用了 `count % SIZE` | 新的一轮运行会跳过开头 → 输出全为空 |

`make test` 之所以抓不到它们，是因为它用自己的 Python 版加载器读环形缓冲，
根本没走启动器的代码。

### 第 3b 层：硬件库、输入路径、流式输出与内存

| 目标 | 内容 | 结果 |
|---|---|---|
| `make test-libs` | `gfx/key/sys/rand/codec/fx` 过模拟器：帧缓冲像素级校验 + 注入按键 + 绘制目标语义 | 19 检查 / 0 失败（库内自检 30+ 项） |
| `make test-input` | 输入路径的安全性质（退出后槽位一定是固件自己的字节、armed 归零、恢复验证、陈旧陷阱修复、`noinputhook` 生效）+ **触摸帧解码**（按下/1 px 抖动被丢/真移动带 dx dy/抬起 + `key.touch()` 的活触点） | 19 检查 / 0 失败 |
| `make test-stream` | **流式输出**：头几行是在"运行中"状态下打印的（不是跑完补的）、半行不露面、**ESC 中断死循环、ON（`KeyboardInterrupt`）中断且启动器存活、ON 落在 sleep 里也接得住、中断能打断阻塞中的 `sys.sleep`、不装钩子时画图循环照样能被打断**、**运行期间一次 PPL `GETKEY` 都不调**、32 KB 环绕不撕裂、`nothread` 退回同步 | 30 检查 / 0 失败 |
| `make test-leak` | 同一会话连跑 4 次 + 连续 4 次"重启启动器" + 重启后旧记录失效 | 无泄漏（每次运行后常驻分配逐字节相同：≈523 KB） |

`test-stream` 用的模拟器侧支持在 **`tests/emu_threads.py`**：primetcc 的模拟器
`svc 0x10000` 只记一个地址（丢掉线程参数和栈，而真机两者都给），`os_sleep` 在
线程里还会被当成"primetcc 的常驻 worker 空闲了"从而结束整轮运行；这个模块
（primeLua 自己的，和 `emu_fixes.py` 一个路数，primetcc 的 checkout 不动）
用一个三指令蹦床把参数和栈交给新线程，并加了"启动器读一次内存 = 让程序跑一会儿"
的可恢复执行模型 —— **"边跑边显示"因此是可断言的，而不是靠看屏幕**。

`test-libs` 读的是**模拟器真的帧缓冲**（`emu.fw.fb_addr`）：红方块中心、
y=100 的绿线、x=200 的蓝线、青色实心三角形、blit 过去的品红 GROB、5x8 文字
的亮像素。按键则通过固件输入槽注入，验证 `key.getkey` 真能拿到扫描码，
并且退出时固件槽已还原（否则下一次调用会崩）。

`test-leak` 证明了镜像复用：连续 4 次重启启动器，固件堆**一个字节都没多**，
`after a reboot` 那一行确认重启后的旧 `lua.slot` 会被拒绝并重新加载。
这两个目标各自独立，因为它们跑的是**真解释器 + 真运行库**（一次 10–20 秒）。

### 第 4 层：固件模拟器端到端差分（`make test`）

`tests/harness_lua.py` 用 primetcc 的 ARMv5 解释器 + 固件 SVC 模型，
**完全按真机启动器的流程**加载 lua.elf（扫 `PLUARING` magic、构造配置块、
`r0=&config` 调入口、抽干环形缓冲），然后把同一个脚本交给主机参考 `lua` 跑一遍，
**两份输出必须逐字节相同**（只归一化路径名与对象地址）。8 个 worker 并行。

```
11/11 scripts passed
  hello.lua          numbers.lua     strings.lua    tables.lua    funcs.lua
  meta.lua           coroutines.lua  patterns.lua   libs.lua      require.lua
  require_default.lua
```

`libs.lua` 覆盖 math/os/io/utf8/debug 全库，`patterns.lua` 专攻模式匹配，
`require_default.lua` 专门验证**打包进解释器的** `LUA_PATH_DEFAULT`
（`C:\DATA\primeLua.hpappdir\?.lua`）在设备路径下能 `require` 到模块。
示例脚本用**路径形式**纳入同一套差分（避免与同名测试脚本混淆）：

```sh
python3 tests/harness_lua.py                 # 全部测试脚本（默认 8 并行）
python3 tests/harness_lua.py -j 1 numbers    # 串行、只看名字含 numbers 的
python3 tests/harness_lua.py examples/sieve.lua   # 路径形式：跑一个示例
python3 tests/harness_lua.py --list
```

`examples/mandel.lua`、`gfxdemo.lua`、`hwcheck.lua` 带 `--# no-compare`：它们用的是
**硬件专有**接口（gfx/key/sys），桌面参考 lua 没有对应物，所以只做"能跑完 + 自检"
这一档，不参与逐字节比对。


参考实现对 **`pairs` 顺序**做过处理：Lua 默认按 `time()` 随机化字符串哈希种子，
primeLua 与参考构建用同一个固定种子（`luai_makeseed`），因此同一个脚本每次运行、
在两种环境下打印的表遍历顺序都一致 —— 这既是差分测试的前提，也是计算器上更想要的
行为（同一脚本每次输出相同）。

---

## 6. 顺手发现的三个模拟器缺陷

调试 §4.3 的日期时，模拟器给出的年份是 370，而 qemu-arm 给的是 1970。
用 qemu 的逐指令 CPU trace 与自建 trace 对比，定位到 primetcc 的 Python 模拟器
（`primetcc/tests/emu_arm2.py`）有 **三个真实缺陷**：

| # | 缺陷 | 症状 |
|---|---|---|
| 1 | **未实现 ARMv5TE 的 DSP 乘法**（`SMULxy/SMLAxy/SMLALxy/SMULWx/SMLAWx`） | 这些指令 bit27-26=00，会**落进"数据处理"分支被当成别的指令静默执行**。GCC 为 `-mcpu=arm926ej-s -O2` 会把 `yoe + era*400` 编成一条 `smlabb` —— 于是所有日期偏到公元 370 年 |
| 2 | **LDM 的基址寄存器也在加载列表里时**（`pop {sp, pc}` 是日常用法） | 模拟器用回写值覆盖了刚载入的值。libgcc 的 64 位除法/浮点辅助例程全是这种 `pop`，于是 64 位除法进入死循环 |
| 3 | **后索引 LDRD/STRD（P=0,W=0）不回写** | LDRD/STRD 的后索引形式 W 位就是 0；`long long` 数组上的 `*p++` 编译成 `ldrd r0, [r3], #8`，指针不前进 → 循环永远读第一个元素 |

**这三个修复没有改进 primetcc**（那是另一个项目，primeLua 不擅自修改它）：
它们放在 `primeLua/tests/emu_fixes.py`，由差分测试在 import 时用猴子补丁打到
模拟器上，`primetcc/tests/emu_arm2.py` 保持原样。三个补丁各自是一个方法的副本 +
一行修正，注释里写了每条的证据与最小复现；若日后 primetcc 自己修了这几处，
删掉 `emu_fixes.py` 即可。

修复后：primeLua 的 11 个端到端脚本全绿，且**指令数与修复前完全一致**；
`/tmp` 中三个最小复现件在模拟器与 qemu 下逐位一致。

> 这也是本项目建议的调试姿势：**模拟器与 qemu 互为参照**。单一模拟器的"能跑"不足以
> 证明代码对，反过来单一实现的"跑不动"也不足以证明代码错。

---

## 7. 构建与部署

### 构建（主机侧，需要 `arm-none-eabi-gcc`）

```sh
cd primeLua
make                 # lua.elf + 部署目录 ../primeLua.hpappdir/
make hostref         # 主机参考 lua（差分测试用，构建在 build/host/）
make test            # 第 4 层：固件模拟器端到端差分（8 并行，约 1.5 分钟）
make test-launcher   # 第 3 层：启动器本身（mock MicroPython + 模拟器）
make test-modes      # 固件文件模式翻译（纯主机，秒级）
make test-dd         # double-double 精度（mpmath 对照，主机，秒级）
make test-libs       # 硬件库 gfx/key/sys/rand/codec/fx（帧缓冲像素 + 注入按键）
make test-stream     # 流式输出：边跑边打印、ESC/ON 中断、32 KB 环绕、nothread 退路
make test-input      # 输入路径的安全性质 + 触摸帧解码（含抖动过滤）
make test-leak       # 连跑 + 连续重启：固件堆不许增长
make test-host       # 第 1 层：主机差分（秒级）
make test-arm        # 第 2 层：32 位 ARM + qemu-arm（需要 gcc-arm-linux-gnueabihf、qemu-user）
make deploy          # 重新生成平铺部署目录
```

### 部署到计算器

`../primeLua.hpappdir/` 是**平铺**目录（计算器不支持子文件夹），整个拷到
`C:\DATA\` 下：

```
C:\DATA\primeLua.hpappdir\
├── lua.elf      Lua 5.4.7 解释器 + 硬件库 + 两族字体 + dd（522 KB，装载 487 KB）
├── main.py      启动器（在计算器的 Python 应用里 import main）
├── *.lua        示例脚本（也可放自己的脚本）
└── api.md / primeLua.md   文档（放机器上随时看）
```

`make deploy` 会把部署目录**先整个删掉再重建**（它是构建产物，不是仓库）：这样一次
干净重建不会把上一版的残留文件（比如以前的 `ti.lua`）留在里面。自己的脚本请放
`C:\DATA\`，别放这里。

**Lua 侧 API 全在 `api.md`**（每个库的函数签名、常量、键码表、例子、已知限制），
本文讲的是移植与验证。

在计算器上：打开 **Python 应用** → 运行 `main.py`（或 `import main`）。
启动器把 `C:\DATA\primeLua.hpappdir\` 与 `C:\DATA\` 下的 `.lua` 文件列成
**一个词典风格的列表画在计算器屏幕上**，选中即运行。

### 计算器端用法（屏幕列表 + 运行时控制台）

选程序在**自绘的列表界面**上（和本工作区的 FileManager / 牛津词典同一套画法：
一个 GROB 缓冲、每帧一次 `BLIT`、文字只用 `textout`、按键轮询
`hpprime.keyboard()` 并以 PPL `GETKEY` 兜底）。列表**没有 footer**：顶栏左边是
`primeLua`，右边显示状态（初始是 `Lua 5.4.7`，跑完一次就变成 `exit 0  12 line`），
下面是条目；程序名在左、字节数右对齐，选中的一行整行反白（蓝底白字），
条目多时右边缘一根细滚动条。

```
┌──────────────────────────────────────────────┐
│ primeLua                        Lua 5.4.7    │  ← 顶栏（状态在右）
├──────────────────────────────────────────────┤
│  fib.lua                             1117 B  │
│  gfxdemo.lua                         3211 B  │  ← 选中：蓝底白字
│  hello.lua                            363 B  │
│  mandel.lua                          4069 B  │
│  sieve.lua                           1175 B  │
│  ─────────────────────────────────────────   │
│  probe / self-test                           │
│  selftest / launch chain                     │
│  log / plua.log                              │
│  rescan folder                               │
│  unload image                                │
│  quit                                        │
└──────────────────────────────────────────────┘   （无 footer）
```

| 键（列表） | 作用 |
|---|---|
| ↑ / ↓（← / → 也能移动） | 移动选择（到头会绕回） |
| ENTER | 运行选中项：清控制台 → 跑（输出边跑边显示，**ESC/ON 可中断**）→ 按任意键回列表 |
| 触摸拖动 | **滚动列表**：手指移动超过 8 px 就算拖动（按行滚动，拖得越远走得越多），抬手不会打开任何行 |
| 触摸点按 | 打开手指按下的那一行（移动不超过 8 px 才算点按 —— 判定放在**抬手**时，所以"想拖动"不会被当成"想打开"） |
| SYMB (F1) | 重新扫描目录（拷进新 `.lua` 之后用） |
| HELP (F3) | 跑 `probe()`：38 项语言/硬件库自检 |
| VIEW (F4) | 显示 `plua.log` 尾部（C 侧自己的诊断） |
| ESC | 退出，回到 Python 终端 |

（分隔线画在行与行之间的空隙里——画在行高中间时会横穿第一条动作的文字，
真机上表现为 "probe / self-test" 那一行上有一条奇怪的灰线。）

**只有运行程序时才用计算器自带的控制台**：ENTER 之后先
`hpprime.eval("print();")`（PPL 无参 `print()` 清终端，primetcc 同一招）把屏幕
交还给控制台，程序的输出经 `moreprint` 彩色打印——**边跑边打**（§7.5），
跑完提示按任意键，再回到列表。跑的时候按 **ESC 可以直接中断程序**（解释器把脚本
`error("interrupted")` 掉，`pcall` 能接住）——就是 primeLua 的 KeyboardInterrupt；
按 **ON** 也一样，只是它走的是固件那条路（ON 是系统中断键，固件会给启动器的 Python
抛 `KeyboardInterrupt`，启动器接住并翻译成同一个中断）。
列表里的动作项（probe / selftest / log）同理，因为它们的结果是**要读的文字**。
等待按键是**有上限**的（约 1200 帧），键盘来源失灵时也会回到列表，不会把机器卡住。

退出（ESC 或 `quit` 行）后回到 Python 终端；**再启动一次**：`main.selector()`、
`main.run()` / `main.run_file("x.lua")`，或按用户手册再运行一次 `main.py`
（在 MicroPython 上 `import main` 也会重新执行——模块在结束时把自己从
`sys.modules` 摘掉）。

**没有屏幕时的控制台菜单**（主机上跑测试、或脚本里想用 stdin 挑程序）：
`main.menu()` 提供同样的选择，用 `> ` 提示符，命令有
`<数字>/<名字>`、`= <lua>` 单行 REPL、`p` probe、`s` selftest、`l` log、
`r` rescan、`u` unload、`g` gc、`h` help、`q` quit。

脚本化用法（不要任何交互界面）：`main.MENU_ENABLED = False`，或者设环境变量
`PLUA_NOMENU=1`，`main()` 就只跑一次 `DEFAULT_SCRIPT` 然后返回。

要改默认选中哪个脚本，动 `main.py` 顶部这个常量（和 primetcc 内嵌代码区一个意思）：

```python
DEFAULT_SCRIPT = "hello.lua"      # 改这里，或用 main.run_file("别的.lua")
```

### main.py 为什么要瘦（`make strip-main`）

计算器的 Python 应用 `import main` 时，**把整个源码编进自己的内存**：实测 156 KB 的
`main.py` 在还没跑任何脚本之前就吃掉约 **6 MB** 堆（比一份 lua.elf 镜像还大）。里面
**45% 是注释和 docstring**——真机上这些字一个都不需要。

所以 `calc/main.py` 现在只有 **87 KB**，散文全部在同目录的 **`calc/main_notes.md`** 里
（按原顺序、按所在函数/类分组）。两边由工具维护：

```sh
make strip-main    # 把 calc/main.py 的注释/docstring 搬进 main_notes.md
make check-main    # 检查 main.py 里又长回散文了（pre-commit 用）
```

工具是**搬移不是删除**：照常在 `main.py` 里写注释，跑一次 `make strip-main`，它把散文
抄进 notes 再从代码里去掉；写完还要重新 parse + compile 一遍，语法不过就不会落盘。

### 输出是流式的：边跑边显示、ESC 能停（§7.5）

**1.0 之前**：解释器在启动器自己的线程上跑，`dbg.call` 不返回就等于 Python 侧什么都做
不了 —— 脚本 `print` 的东西要等它**跑完**才整块出现，跑飞了也没有任何办法（只能拔电池
或等它自己结束）。

**现在**：`plua_entry` 起一个固件线程跑解释器，把运行状态交回启动器后**立刻返回**，
启动器于是这样循环（`calc/main.py: stream_run`）：

```
每 25 ms（固件 os_sleep，让出 CPU 给正在跑的程序）：
  · 读环形缓冲里 count 之后的字节 → 按行打印（moreprint 彩色）
  · 状态不是「运行中」→ 收工
  · 看键盘：**ESC**（启动器这一侧是 4，`hpprime.keyboard()` 的位掩码
            `mask & (1<<4)`，不碰 PPL）→ 把运行状态的 abort 置 1
  · 收到 Python 的 KeyboardInterrupt → 那是 **ON** 键（见下表），同样置 abort
  · 自己让出 CPU：每轮一次固件 `os_sleep`（走调试通道）
```

解释器那边，abort 由一个 **Lua 调试钩子**发现（`port/plua_hp.c`，每 20000 条 VM 指令
一次），它做的就是在 Lua 里 `error("interrupted")`：脚本于是像自己报错一样解开——
`pcall`、`xpcall`、to-be-closed 变量全都按语言的规矩走，退出码和错误信息照样经环形
缓冲回到启动器：

```
> main.run_file("spin.lua")
[*] running spin.lua ...
spinning
lua: .../spin.lua:4: interrupted
stack traceback:
        ...
[!] interrupted (ON)
```

**真机踩坑记录（1.0 第一次上机：跑什么程序都立刻复位）**：`port/plua_run.c` 里那两个
固件调用一开始是"按签名猜"写的——`create_thread(fn, &config, 512KB)` 把配置指针塞进
r1，`set_thread_priority(60)` 把优先级放在**第一个**参数。对着 `puredoom.elf` 的
`main()` 反汇编逐字核对后才知道：

```
r0 = 线程体, r1 = 0（不是用户参数！DOOM 传 0，线程从全局读状态）, r2 = 0x80000
bl create_thread                 ; 返回句柄仍在 r0
mov r1, #60
bl set_thread_priority           ; 句柄在前、优先级在后
```

也就是说我们是让固件"把 60 号线程的优先级设成一个野值"，于是一运行就复位。
现在这两行是照着 DOOM 抄的，代码里也写了"别顺手改"。**教训**：固件的 SVC 参数
次序在别处没有文档，只有"能跑的那个二进制"是权威 —— 先反汇编，再写调用。

**几条边角，都是刻意的**：

| 情形 | 行为 |
|---|---|
| 一行只写了一半（`io.write("a")` 后睡一会儿再写 `"b\n"`） | 不打印，攒到换行再说 —— 控制台上看不到"半行" |
| 输出比 32 KB 还快（两次轮询之间被覆盖） | 只打印还活着的部分，第一条可能是半行；环形缓冲存的是字节，没有行边界 |
| **中断要不要付费：`interrupt` 标记** | 真正让"任意脚本都能被 ON 打断"的是一个 Lua 调试钩子，而钩子会让 VM **每条指令**都多调一次 `luaG_traceexec`——模拟器实测 fib(24)：**不装钩子 25,009,938 条指令，装钩子 52,083,122 条（+108%）**，真机上就是 fib(30) 的 3 秒变 5 秒。所以默认**不装**：print/io.write 和每个 gfx 绑定自己查中断字（一次内存读），画图循环、打印循环、sleep、等键都能被打断；**纯计算循环（fib 这种）默认打不断，会跑完**。要"什么都能打断"，在应用目录放一个空文件 **`interrupt`**（代价就是那 ~2 倍）。`plua_launcher.log` 里会写 `[stream] abort hook ARMED` 或 `off` |
| 中断键：**ESC**（启动器读得到的那个），**ON** 也管用 | ON 是这台机器的系统中断键：按下它，**固件直接给正在跑的 Python 程序抛 `KeyboardInterrupt`**——真机实测就是这样，控制台打印 Python 的 traceback、启动器当场死掉，而脚本在**自己的固件线程**上继续画 mandel。所以启动器现在**接住**它并翻译成同一个 abort（`[!] interrupted (ON)`）；而它读得到的按键中断是 ESC（`STREAM_ABORT_KEYS = (4,)`，想换键改这一行；19 = 退格是备选）。ESC 留给脚本的那点自由没了，但"能停下来"更重要 |
| 中断有多快 | 每次轮询都查键：有输出在流动时 50 ms 内被读到，程序闷头算时 150 ms（空闲轮询放宽到 150 ms —— 见下面那条）。按 ON 也一样快：它走系统中断那条路（见上一行） |
| ON 落在哪 | 真机上 ON 的 `KeyboardInterrupt` **落在启动器 sleep 的那一刻**（轮询的 129 ms 里几乎都在那儿）——所以那个 `os_sleep` 调用必须在 `try` 里面。第一版把它放在 `try` 外面，于是 ON 变成"未捕获的 Python 异常"，启动器死了、脚本继续画（`make test-stream` 有一条专门模拟"睡眠中被打断"的用例） |
| **跑得比原来慢** | 启动器和解释器是**一颗 CPU 上的两个线程**，轮询本身就是抢时间。1.0 最初每 25 ms 轮询一次、而且每次都调 PPL 的 `GETKEY`（要过表达式求值器）——真机实测 mandel 只剩不到一半速度。现在：**程序运行期间一次 PPL 都不调**（只用 `hpprime.keyboard()` 的位掩码 `keyboard() & (1<<id)`）、每次轮询都查键、没有输出时轮询间隔自动放宽到 150 ms、每轮轮询的调试通道往返从 3 次降到 1~2 次。`plua_launcher.log` 里会写实测的 `ms/poll`（真机实测 128.8 ms/poll = 确实在睡，没有空转），睡不着会额外打一行 `WARNING: polls are not sleeping`。**GUI 里照旧可以用 `GETKEY`**（那时机器上只有启动器在跑，`KeyPad` 类就是给 GUI 用的），只有"Lua 在跑"的这段路禁止 |
| 流式税实测（真机） | `examples/bench.lua`，同样的 20 万次循环：**流式 0.571 s / `nothread` 同步 0.561 s** —— 差 10 ms，约 **1.8%**。修之前那一版是"慢一半以上"；差别就是每轮一次的 PPL `GETKEY`。要复现：跑一次 `bench.lua` 记下秒数，在应用目录放空文件 `nothread` 再跑一次 |
| 脚本正卡在一次阻塞调用里（`sys.sleep(4000)`、`key.getkey(4000)`） | 也能中断：这些绑定把等待**切片**（每 20 ms 查一次 abort 字）再抛同一个 `error("interrupted")`，所以最坏 20 ms 就响应（`make test-stream` 里有一条专门从 `sys.sleep` 里中断的用例） |
| 脚本用 `pcall` 接住了 `interrupted` | 那是它的自由：这个错误按 Lua 的规矩走，`xpcall`/to-be-closed 变量一样。它就是 primeLua 的 KeyboardInterrupt，可捕获 —— 清理本来就是脚本自己的事 |
| 一次输出超过 32 KB | 启动器按 `count` 追，追不上就跳到还活着的最旧字节（不会把两个时代的字节混着打印） |
| 不想用线程（万一某个固件不喜欢） | 在应用目录放一个空文件 **`nothread`**：回到"跑完再一次显示"的老行为，功能不变 |
| 判断是哪种模式 | `plua_launcher.log` 里的 `[stream] mode=thread run=0x... ring=0x...`、`[stream] calling entry 0x...`、`[stream] entry returned 0x...`，`plua.log` 里的 `thread: entered` / `stack: thread bytes=00080000` / `thread: done` |
| 万一真机上线程不能跑 | 在应用目录放一个空文件 **`nothread`**：回到旧的同步执行（入口自己跑完才返回），输出在跑完后一次显示，其余功能不变。日志里会写 `mode=inline` |

`make test-stream` 把这些都变成断言：**输出的头几行是在运行状态还写着"运行中"的时候
打印的**（不是跑完补的）、半行不露面、ESC/ON 能让死循环退出并带上 `interrupted`、
32 KB 环绕后不撕裂、`nothread` 退回同步模式仍然完整。测试用的模拟器侧支持在
`tests/emu_threads.py`（`svc 0x10000` 真的给线程**参数和栈**，以及"启动器读一次内存＝
程序跑一会儿"的可恢复执行模型）—— primetcc 的模拟器保持原样。

### 解释器镜像只加载一次（`lua.slot`）

一台机器上连续跑几次，固件剩余内存是这样的（用户实测，**每跑一次少 0.4–2 MB**）：
`15386 KB → 13194 KB → 12808 KB`。原因是每次重新 `import main` 都会再往固件堆里
**加载一份 500 KB 的 lua.elf**，而 MicroPython 的模块对象随 `sys.modules` 一起
消失，旧镜像就再也没人释放了（primetcc 用 `tcc.slot` 解决的是同一个问题）。

primeLua 现在把镜像位置记在 `lua.slot`（magic `AULP`）里：下次会话**先核对再复用**
——核 `PLUARING` 指纹、镜像首字的 ELF 头、入口点第一条指令——对得上就直接用，
对不上（重启过、内存被固件重新分配过）就丢掉记录重新加载。记录里还存着**磁盘上
`lua.elf` 的指纹**（大小 + 头/中/尾三段各 32 字节的校验和），于是**换了一版 lua.elf
不会再被常驻的旧镜像挡住**：指纹对不上就把旧镜像交还固件堆、重新加载新的那份。
没有这一条的话，升级后会继续跑上一版解释器（`sys.version()` 还是老号码），看起来
"换了但没生效"——这是最容易被误判成"白忙一场"的坑。效果（模拟器实测）：

```
runs 1..4 identical: ~523020 bytes live  ← 每次运行结束后常驻分配逐字节相同
session 2/3/4:  live=~523020，image_base 与第一次相同（镜像被复用，0 字节新增）
after a reboot: exit=0  stale-slot=False（旧记录被拒绝，重新加载）
after replacing lua.elf: adopted-old=False，image 换到新地址，live 不变（旧镜像被释放）
                         （旧镜像被释放，没有多出一份）
```

**怎么读这些数字**：`live`（常驻分配）才是泄漏计——每次运行后完全一致，说明
"跑完什么都不留"；**高水位**（high-water）只做参考，模拟器的固件堆是 bump +
简单 free list，偶尔会有 1–3 KB 级的碎片漂移（实测跑 8 次里涨了 3 次，之后停住），
不是泄漏——真丢一份镜像的话那一次就是 **+500 KB**，检查里用 32 KB 的余量卡这个。
`make test-leak` 就是这套检查（重启、换 lua.elf 两种场景都在里面）。真机上如果怀疑
内存被占，列表里选 `unload image`（或调 `main.unload()`）释放镜像。

**升级到新版时**：把新的 `lua.elf` 连同脚本一起拷进 `C:\DATA\primeLua.hpappdir\`，
启动器会自己发现文件变了并重新加载 —— 不需要重启计算器，也不需要先 `unload image`。
换完在终端里 `print(sys.version())` 对一下版本号（这一版是 `1.0`）。

### 控制台接口

```python
import main
main.selector()                 # 进入屏幕列表（import main 默认就是这个）
main.menu()                     # 没有屏幕时的控制台菜单（stdin 提示符）
main.run_file("fib.lua")        # 跑一个脚本，输出经 moreprint 彩色打印
main.run()                      # 跑 DEFAULT_SCRIPT
main.run_source("print(1/3)")   # 跑一段源码（写临时文件后运行）
main.list_files()               # 列出发现的脚本
main.probe()                    # 38 项自检（含硬件库），逐项 pcall
main.selftest("sieve.lua")      # 逐段检查整条加载链
main.unload()                   # 归还解释器镜像占用的内存，并作废 lua.slot
```

颜色约定（与 primetcc 一致）：`p_log` 启动器信息、`p_out` 脚本自己的输出、
`p_error` Lua 报错与加载失败、`p_pass` 成功行、`p_warning` 需要留意的事。
`moreprint` 缺失时自动退化为 `print`，功能不受影响。

---

## 8. 与桌面 Lua 的差异 / 已知限制

这些都是**设备物理限制**导致的，不是没做完：

| 项 | 说明 |
|---|---|
| **输出只有最后 32 KB** | 环形缓冲大小所限；`for i=1,1e6 do print(i) end` 只能看到尾部。写得太快时两次轮询之间被覆盖掉的字节里，第一条可能是半行——环形缓冲存的是字节，没有行边界（`make test-stream` 把这条也钉住了） |
| **启动器绝不写死地址** | 真机上写一个硬编码地址可能直接复位整机：`0x30000000` 在模拟器里是应用内存起点，在真机上属于固件。selftest 原来第一步就往那里写 0 → 真机**一运行 selftest 立即重启**；现在改为"向固件 malloc 给我们的块写/读回"，`test_launcher.py` 里有一条断言：调试通道的每一次写都必须在固件堆范围内 |
| **运行时可以中断，但不能交互输入** | **ESC（或 ON）** 会中断正在跑的程序（§7.5），但控制台本身不接受输入：脚本要键盘就用 `key.getkey()`；真正的交互式终端需要 Python 侧与程序同时读写屏幕，那是另一件事 |
| `os.clock()` **只走时、不走 CPU 时间** | 计算器只有一个进程，所以它给的是"解释器起来以后过了多少秒"（固件计数器现场标定，见 §4.4）。固件时钟不可用时返回 0 —— 不编 |
| `os.date/os.time` 依赖固件或启动器 | 先是固件实时钟（svc 0x100A5），其次启动器每次运行前从 PPL `Date`/`Time` 读的那个 epoch；两个都没有时返回 0（1970-01-01），不做假 |
| `os.getenv()` 恒返回 nil | 计算器没有环境块（因此 `LUA_INIT` 不会生效） |
| `os.execute` / `package.cpath` / `require` C 模块 | 无进程、无动态库；`package.cpath` 为空串，`os.tmpname` 固定在应用目录 |
| `io` 是固件文件 API | 路径为 `C:\DATA\...` 风格；**相对路径会被解释器解析到脚本所在的 app 目录**（固件自己的 cwd 不是那里，所以 `io.open("x","w")` 若不这样处理会报 No such file or directory）；无 `rename`；无 `io.popen` |
| `os.rename` 返回失败 | 固件没暴露对应服务 |
| 字符串哈希种子固定 | 见 §5：为可复现性放弃了 Lua 的防哈希洪泛随机化（计算器上无关紧要） |
| 没有真正的 REPL | 逐行求值的 REPL 需要输入源；CLI 菜单里的 `= <lua>` 是可用的单行 REPL，多行用 `main.run_source()` |
| 图形/键盘 API | 已提供 `gfx`/`key`（见 §10）。图形程序**运行期间屏幕照常刷新**（帧缓冲由 Lua 自己 blit，固件 LCD 一直在扫），输出现在也是边跑边显示（§7.5） |
| 输出为 UTF-8 字节 | 计算器屏幕对非 ASCII 的显示取决于启动器字体支持 |

**真机排错**（都在 Python 终端里跑，输出同样是彩色的）：

```python
import main
main.probe()                   # ① 一次运行逐项自检 38 个语言/库特性（每个都 pcall），
                               #    哪项 FAIL 会直接打印 Lua 的错误原文
main.selftest("sieve.lua")     # ② 逐步检查整条链路：调试接口→固件堆→目录→ELF 头
                               #    →上传 shellcode→加载镜像→找环形缓冲→跑脚本
main.list_files()              # ③ 确认它到底列出了哪些脚本
```

**掉电不丢的诊断日志**：解释器把每一步写进 `C:\DATA\primeLua.hpappdir\plua.log`
（配置指针、固件剩余堆、调试栈、拿到的程序栈大小、启动/返回/退出码），每次写完整行后
**关闭文件**——所以**复位后日志还在**（控制台和输出环形缓冲都会没）。启动器会在运行失败
时把日志尾部贴到输出页；按 `help` 也能看到最近几行。日志长这样：

```
---- run ----
cfg=31060610
heap_free=00800000        ← 固件当时还剩多少堆（字节）
dbg_sp=31ffbfd0
argc=00000002
stack: thread bytes=00080000  ← 程序跑在固件线程上，栈是固件给的（0x80000 = 512 KB）
lua: starting
lua: returned status=00000000
exit code=00000000
```

`stack: malloc failed at ...` 或出现静态回退，就说明固件堆不够；`heap_free` 太小则
脚本会在 Lua 自己的堆分配上失败（报 "not enough memory"）。

**已记录但不影响的偏差**：`%#g` 在进位跨十进位时的尾零（primeLua 按 C11，glibc 不按），
见 §4.1 的 2 条记录。

---

## 9. 目录结构

```
primeLua/
├── Makefile                     构建/测试/部署（唯一入口）
├── README.md                    本文（移植、测试、真机排错）
├── api.md                       **Lua 侧 API 参考**（所有库：签名/常量/例子/限制）
├── src/lua-5.4.7/               上游 Lua 源码（未打任何补丁）
├── port/                        primeLua 平台层（移植的难点都在这里）
│   ├── plua_svc.s               固件 SVC + setjmp/longjmp + 入口
│   ├── plua_main.c              启动粘合、程序栈、exit 展开、RT_RET
│   ├── plua_run.c / .h          **跑在固件线程上**：svc 0x10000 起线程、
│   │                             运行状态（magic PLUARUN）、abort 字（1.0 新增）
│   ├── plua_format.c            C99 printf 引擎（精确十进制）
│   ├── plua_strtod.c            正确舍入的 strtod
│   ├── plua_file.c              FILE 层 + 环形缓冲 + 固件文件 API
│   ├── plua_libc.c              realloc/exit/errno/locale/ctype/strerror
│   ├── plua_math.c              标准名 → openlibm
│   ├── plua_time.c              64 位 time_t、gmtime/mktime/strftime、**固件实时钟**
│   ├── plua_hp.c                硬件库绑定（gfx/key/sys/rand/codec/fx/font/dd → Lua）
│   ├── plua_input.c / .h        **输入层（自己的，DOOM 的机制）**：get_event 槽位钩子、
│   │                             事件/触摸解析、槽位恢复与验证
│   ├── plua_font_montserrat_data.c  Montserrat 16/24/32 px 字模（generated：tools/make_font_montserrat.py）
│   ├── plua_font_cascadia_data.c    Cascadia Code Light 16/24/32 px 字模（generated：tools/make_font_cascadia.py）
│   ├── plua_font_big_data.c        钟表的 56 px 数字面（generated：tools/make_font_big.py，`make font-big`）
│   ├── plua_dd.c / plua_dd.h    double-double 算术与超越函数（新库 dd）
│   ├── plua_dd_NOTES.md         dd 的算法、实测精度与已知取舍
│   ├── plua_port.h              启动契约（struct plua_config）与内部声明
│   ├── include/                 遮蔽 newlib 的头文件
│   └── openlibm-extra/e_log2.c  primetcc 的 openlibm 子集缺的那个文件
├── calc/
│   ├── main.py                  计算器端启动器（MicroPython）
│   ├── timeprobe.py             真机时间源探针（怎么量出 svc 0x100A5 的，见 §4.4）
│   └── timeprobe.log            2025-09-25 那次真机测量的原始输出
├── examples/                    示例脚本（随部署目录一起拷）
├── tests/
│   ├── harness_lua.py           端到端差分（模拟器 vs 参考 lua，多进程）
│   ├── test_format_host.c       printf 差分（主机 + ARM）
│   ├── test_strtod_host.c       strtod 差分（主机 + ARM）
│   ├── test_time_host.c         strftime 差分 + 真机时钟结构解码（主机 + ARM）
│   ├── emu_time.py              svc 0x100A5（模拟器侧的固件时钟）
│   ├── emu_fixes.py             给 primetcc 模拟器的三个补丁（不修改 primetcc）
│   ├── test_launcher.py         启动器测试（mock MicroPython：拒绝绝对路径 + 屏幕列表 + 控制台菜单）
│   ├── test_hp_libs.py          硬件库过模拟器（帧缓冲像素 + 注入按键）
│   ├── test_input.py            输入路径的安全性质（跑完/跑挂都不许留下补丁）
│   ├── leak_check.py            连跑/重启不涨内存（lua.slot 复用镜像）
│   ├── qemu_arm/run_arm_tests.py 在 qemu-arm 下跑上面三套
│   ├── scripts/*.lua            12 个差分脚本（hp_libs.lua 是设备专用，自检）
│   └── modules/pluamod.lua      require 用模块
└── tools/
    ├── fix_relocs.py            构建期把符号重定位折叠成 R_ARM_RELATIVE
    ├── make_font_montserrat.py  重新生成 Montserrat 字模（primetcc 表 → port/）
    ├── make_font_cascadia.py    重新生成 Cascadia 字模（离线栅格化 → port/）
    ├── make_font_big.py         重新生成钟表数字面（56 px / `BIGSIZE=n`）
    ├── gen_dd_expected.py       dd 的 mpmath 60 位期望值
    └── make_dist.py             打 dist/ 分发包

../primeLua.hpappdir/            平铺部署目录（拷到计算器 C:\DATA\）
../_attic/primeLua-ti/           ti 库（TI-Nspire 兼容层），这一版不做，见 §1
../_attic/primeLua-touch/        原始多点触摸帧的测试与模拟器支持，这一版不带，见 §12
```

---

## 10. 硬件库（`gfx` / `key` / `sys` / `rand` / `codec` / `fx`）

primetcc 的运行库被编译进 primeLua，注册成 Lua 全局模块，**同时**在
`package.preload` 里放一个**加载函数**，所以 `gfx.line(...)` 和
`local gfx = require("gfx")` 都能拿到，而且拿到的是**同一张表**。

> 踩过的坑：`package.preload` 里必须放**函数**，放表本身不行——Lua 的 preload
> 搜索器把结果交给 `findloader()`，它只接受函数，否则继续找下一个搜索器，于是
> `require("gfx")` 会报 `no file 'C:\DATA\gfx.lua'`（而 `gfx.line` 却好好的）。
> 现在 `register_lib()` 用闭包把表包成加载函数，`gfx`/`font`/`sys` 都验证过。
函数名一律用 Lua 世界的常规写法（`gfx.fillrect`、`key.getkey`、`codec.crc32`），
不加 `hp_` 前缀。

```lua
local gfx, key = gfx, key

gfx.init()                            -- 建默认全屏 GROB 并接管绘制目标
gfx.clear(gfx.BLACK)
gfx.fillrect(10, 10, 100, 40, gfx.RED)
gfx.line(0, 0, 319, 239, gfx.WHITE)
gfx.circle(160, 120, 40, gfx.YELLOW)
gfx.text(4, 4, "hello", gfx.WHITE)
gfx.blit()                            -- 后缓冲整屏送到屏幕（一次拷贝，无闪烁）

local g = gfx.newgrob(64, 48)         -- 离屏位图 = 双缓冲
gfx.select(g)  gfx.clear(gfx.CYAN)    -- 画到 GROB 上
gfx.select()                          -- 回到默认后缓冲
gfx.blit(g, 240, 160)                 -- 把 GROB 拷到当前目标
gfx.blit()                            -- 再整屏呈现
gfx.freegrob(g)

key.install()                         -- 挂上按键钩子（默认就是开的；退出时自动还原固件槽）
local kc = key.getkey()               -- 设备键码；无按键时是 nil（不是 0）
local ev = key.event()                -- 键盘/触摸都在这条队里（触摸带 x y dx dy）
local down, x, y = key.touch()        -- 当前有没有手指按着、按在哪
key.remove()
```

| 模块 | 主要内容 |
|---|---|
| `gfx` | `init/width/height/rgb/clear/pixel/getpixel/line/hline/vline/rect/fillrect/circle/fillcircle/triangle/filltriangle/text/textbg/textwidth/blit/blitpixels/newgrob/freegrob/select/grobwidth/grobheight`，颜色常量 `BLACK…GRAY`、`WIDTH`/`HEIGHT` |
| `key` | `install/remove/hooked/hookstats/event/getkey/gettouch/touch/sleep`，按键常量见下面的**键码表**；触摸的语义（phase 1/2/8、dx/dy、抖动过滤）见 §7 的输入小节 |
| `sys` | `memory`（剩余内存）、`maxalloc`、`sleep`、**`time()`**（固件实时钟的 Unix 秒，没有则 nil）、**`clock()`**（解释器起来以后过了多少秒）、**`log(text)`**（写进持久日志 `plua.log`，复位也不丢）、**`version()`**（解释器 API 版本，见下） |
| `rand` | `seed/u32/range/noise` |
| `codec` | `crc32/adler32/hex/unhex/base64/unbase64` |
| `fx` | Q16.16 定点（无 FPU 时更快）：`int/trunc/round/todouble/fromdouble/neg/abs/add/sub/mul/div/lerp/sqrt/sin/cos/tan/atan2` | — |
| **`font`** | 真正的字体渲染（primetcc 引擎 + **Montserrat（比例）/ Cascadia Code Light（等宽），各 16/24/32 px**）：`prop mono cascadia height width advance draw drawbg` | 见下面的字体小节 |
| **`dd`** | double-double（≈31 位十进制）**新库**，`math` 一行没动：构造 `dd(x)`、运算符 `+ - * / ^ == < <=`、`pi/e/ln2/ln10`、`sqrt exp log log10 sin cos tan asin acos atan atan2 pow fmod hypot abs floor ceil neg`、`tostring(x[,位数]) tonumber parts isnan isinf new` | 见下面的 dd 小节 |

#### 绘制模型（`gfx.init()` 之后就在屏幕上了）

| 调用 | 作用 |
|---|---|
| `gfx.init()` | 初始化（读固件 LCD 结构），**绘制目标＝屏幕本身**：画了就能看见，不需要 blit，也**不分配** 307 KB 后缓冲 |
| `gfx.select()` | 按需**分配**默认后缓冲（307 KB），并用**当前屏幕内容做种子**（所以第一次 `blit()` 不会把之前画的东西抹掉）；之后画到缓冲、`blit()` 上屏 |
| `gfx.select(false)` | 回到直接画屏幕 |
| `gfx.select(g)` | 画到某个 GROB（离屏合成） |
| `gfx.blit()` | 把默认后缓冲整屏拷贝上屏（没有后缓冲时是空操作——画本来就在屏幕上） |
| `gfx.blit(g, x, y)` | 把 GROB 拷到**当前绘制目标**（不切目标） |
| `gfx.freebackbuf()` | 释放后缓冲（307 KB 还给固件堆），回到直接画屏幕 |
| `gfx.target()` | `"screen"` / `"default"` / `"grob"`：现在画到哪儿 |
| `gfx.fb()` | 固件 LCD 结构里的 `framebuffer, w, h`——排错用（整屏绘制出事时第一个要问的就是这个地址对不对） |

后缓冲**按需分配**是故意的：307 KB 在固件堆里不是小数目，而只画屏幕的脚本根本不需要它；
primetcc 的注释里也记着这块固件对"大块 alloc/free"最不可靠。写回缓冲时目标绝不会悬空：
`freegrob()` 释放的正是当前目标时会先切回默认后缓冲（否则下一次 `clear()` 会写进已回收的内存）。

`key.getkey([wait_ms])` 返回**固件的设备键码**；没有按键时返回 **`nil`**（会跳过
按键释放事件，等 `wait_ms` 毫秒后仍无键也返回 nil）。所以取键循环写成
`local kc = key.getkey(); if not kc then ... end`——写成 `== 0` 会永远不成立。
`key.gettouch()` 读触摸事件（坐标系与 `gfx` 相同）。

#### 键码：项目里有**两张不同的表**（踩过一次坑）

| 键 | **设备键码**（Lua `key.getkey()` 返回、`key.*` 常量） | PPL `GETKEY` / `hpprime.keyboard()` 位号（Python 侧用） |
|---|---|---|
| esc | `0x01` (1) | 4 |
| ← 左 | `0x02` (2) | 7 |
| ↑ 上 | `0x03` (3) | 2 |
| → 右 | `0x04` (4) | 8 |
| ↓ 下 | `0x05` (5) | 12 |
| 退格 | `0x0C` (12) | 19 |
| 回车 | `0x0D` (13) | 30 |
| 空格 | `0x20` (32) | 49 |
| ON | `0x83` | **46**（系统中断键，见下） |
| APPS / SYMB / PLOT / NUM / VIEW / CAS / MENU | `0xB1/0x91/0xB2/0xB3/0xB4/0xB5/0x93` | 0/1/6/11/9/10/13 |

设备键码来自 primetcc 的 `rt/hp_input.h`（对着计算器扫描码矩阵逐项核对过），
primeLua 的 `key.*` 常量直接用它那套宏注册，所以不会和运行库脱节。

**ON 那一行有点特别**：它是这台机器的**系统中断键** —— 按下它，固件不是把键交给应用，
而是给正在运行的 Python 程序抛 `KeyboardInterrupt`。所以启动器"读"不到它（要用
`try/except KeyboardInterrupt` 接），而流式显示的中断键用 **ESC**（启动器这一侧 4）。
脚本按 `key.ON`（`0x83`）/`key.ESC`（`0x01`）认这两个键 —— 同一颗键在不同表里
不同编号，正好是这张表存在的理由。

**为什么值得单列一张表**：曾经把 PPL `GETKEY` 的编号当成 `key.*` 常量注册，
于是**按右键却退出程序**——右方向键的设备键码 `0x04` 正好是 `GETKEY` 里 esc 的
编号 4；按左键变成"向上"（`0x02` vs `GETKEY` 的 up 2），上下键则完全没反应
（3、5 不在表里）。`examples/gfxdemo.lua` 直接用 `key.LEFT/key.RIGHT/...`，
`tests/scripts/hp_libs.lua` 里有一项专门钉死这张表（并且注入右键验证它**不等于**
`key.ESC`）。

#### 输入层：DOOM 的机制，primeLua 自己的代码（`port/plua_input.c`）

**键盘/触摸这条路换过三次，前两次都是在真机上把输入弄死的：**

| 版本 | 做法 | 真机结果 |
|---|---|---|
| 1 | 编译 primetcc 的 `rt/hp_input.c` + `rt/hp_input_svc.c`（共享源码，为另一个项目的触摸改过两次） | 最近一次改动把写槽位的 cache 维护扩成"整块 D-cache 清+失效 + 整 TLB 失效"，**一调 `key.install()` 就复位**（"gfxdemo/clock 一进去就重启，mandel 正常"）。模拟器完全无感：没有 cache、没有 TLB、没有 MMU |
| 2 | primeLua 自己的"裸汇编入口 + 手工搭跳板" | `bisect_hookslot.lua`（装+拆，系统从未进入）输入正常，`bisect_hookentry.lua`（装，等 3 秒，拆）**输入死** —— 结论当时写成"从我们的代码里过一趟本身就致命" |
| **3（现在）** | **PureDOOM 的机制**，从 `puredoom.elf` 反出来重写 | DOOM 在这台机器上用同一个槽位读键盘和触摸，一直好用 —— 说明第 2 版的结论是错的，错在自己那套写法上 |

第三条是这一版的全部内容。**跟 DOOM 一致的地方，逐条对应**（`puredoom.elf` 的
`install_input_hack` / `my_get_event_hook` / `aggressive_memcpy`）：

| 事项 | DOOM 怎么做 | primeLua 现在怎么做 |
|---|---|---|
| 钩子入口 | 普通 C 函数 `my_get_event_hook`，被槽位**跳转**进入（`ldr pc,[pc,#-4]`），返回时用调用者的 LR | 同样：`plua_input_hook()` 是普通 C 函数，`pop {…, pc}` 回到 OS |
| 真正取事件 | 调自己的 `sys_get_event()`：`push {r0}; push {lr}; svc 0x1003F` ——**不是**跳到固件桩的副本 | 一样：调 `port/plua_svc.s` 的 `hp_svc_get_event`。**没有跳板了**，上一版那套"手工复制固件 16 字节再 blx"整段删除 |
| 打补丁 | 只写 **8 字节**：`e51ff004` + 入口地址 | 一样（上一版写 16 字节，把固件桩的后面两句也覆盖了） |
| 写槽位的姿势 | `aggressive_memcpy`：关 IRQ → 把 TTBR 写成全 1 让核静默 → 写 → 清+失效 D-cache → 失效 I-cache → 清写缓冲 → 还原 TTBR | 一样（`slot_copy()`，CP15 指令逐条照抄）。上一版缺"静默"这一步，而 README 早先把它记成"整块 D-cache 失效 = 复位"，其实差的是这一步 |
| 事件解析 | 就在 OS 输入线程上解析（循环、写全局、调函数） | 一样：`plua_input_hook()` 里直接解析进 Lua 侧队列。上一版把解析挪到 Lua 线程，理由是"过一趟就致命"——那个理由不成立 |
| 按键 | 按 `flag == 16`（按下）/ `0x100000`（松开），`+34` 是设备扫描码 | 一样（设备扫描码直接进队列，不再映射成别的编码） |
| 触摸 | `+24` 是触点个数（≤8），每项 12 字节：`+0` 动作(1/2/8)、`+4` 必须是 0、`+6` x、`+8` y；**移动小于 1 px 丢掉**；按下的位置记在 `g_last_touch_x/y` | 一样（抖动过滤结果进 `hookstats().touch_jitter`，被接受的移动进 `touch_moves` 并带 `dx/dy`） |

**为什么槽位是同一个**：固件的 `get_event`（svc 0x1003F）是**阻塞**的、归操作系统的
输入线程所有，从应用线程直接调会把机器冻住（真机验证过）。所以这台机器上的所有项目
（PureDOOM、primetcc、primeLua）都是**改固件 API 槽位 + 观察事件**，而不是轮询。槽位就是
固件自己的 `get_event` 桩的第一条指令：

```
0x307FBFA0:  e52d0004   push {r0}        <- 槽位（只覆盖这一条 + 下一条地址字）
0x307FBFA4:  e52de004   push {lr}
0x307FBFA8:  ef01003f   svc  0x1003F
```

（对着本工作区自己的 App Disk 核过：`PRIME_APP.DAT` → `PROGRAMS/MISC/ARMFIR.ELF`，
桩表每 12 字节一条，从 `0x307FBF1C` 起。）

**primeLua 自己的部分**（DOOM 没有的一半）：队列（`key.event/getkey/gettouch/touch`）、
`key.hookstats()` 的计数器、启动器用来"收工"的状态布局（magic `PRIMEIN`），以及
`key.install()`/`key.remove()` 这对显式开关。

**六条安全性质（`make test-input` 钉死）**：

| # | 性质 | 为什么 |
|---|---|---|
| 1 | **事件只读** | 操作系统看到的就是内核写进去的那几个字节。改调用方的事件（被替换掉的那份代码里的"synthetic contact"）等于编辑别人正在消费的输入 |
| 2 | **没装钩子 = 什么都不做** | 槽位是固件自己的字节时，OS 根本不会进我们的代码；`armed=0` 时钩子也只是把事件原样放行（`get_event` 必须照调，否则操作系统的输入就断了） |
| 3 | **恢复要回读验证** | `remove()` 把固件字节写回去、**再读回来比对**，失败重试一次，结果进计数器。悄悄失败的恢复从此是一个数字，不是一个悬案 |
| 4 | **原始字节不可信就用固件桩** | 槽位里已经是我们的陷阱、又没有可信原件时，写回固件那 4 个字（`PLUA_STUB0..3`）。上一版要求的"跳板"这一版不存在，所以没有"钩子自己调自己"这条路 |
| 5 | **只做这台机器验证过的 cache 维护** | DOOM 的 `aggressive_memcpy` 那三条 CP15 + TTBR 静默，一条不多。整块 TLB 失效不是保险，是复位 |
| 6 | **只有程序在监听时才打补丁，退出时恢复** | 包括**没调 `key.remove()` 就退出的程序**：启动器每次运行后都会核对槽位（`calc/main.py`） |

**收尾的一方（启动器）必须找得到这块状态。** 真机日志暴露过这条链上最要命的一环：

```
[hook] stand-down: no PRIMEIN in the image
[hook] NOT disarmed after clock.lua
```

启动器原来靠**扫描 lua.elf 找 `PRIMEIN` 魔数**定位状态，而那个扫描在真机的文件 API 上
**根本不工作**（要顺序读 400+ KB）。于是"程序退出后一定要把固件的输入交还回去"这条兜底
**每次都是空转**：程序没调 `key.remove()`（或中途出错）时，钩子留在槽位里、`armed` 还是 1，
触摸会一直往一个没人读的队列里塞——正是"退出后一摸屏幕，按键和触摸全死"。

现在解释器在启动时把状态地址写进 `plua.log`（`in_state=305217F8`，`port/plua_main.c`），
启动器读这一行就够了，不再扫文件（`calc/main.py: find_state_addr`）。另外补了一条硬规矩：
**槽位里还是我们的陷阱时，绝不释放镜像**——恢复不了就宁可漏掉那 500 KB，也不让操作系统的
输入路径指向一块马上要被交出去的堆内存（`hand_back_input_before_free`）。`make test-input`
把"扫描坏掉时 stand-down 仍然成功"这一条也钉死了（照着真机那个失败模拟的）。

**拦截默认是开的**，关掉的办法是在应用目录放一个空文件 **`noinputhook`**（启动器启动时
把它写进输入状态里的策略字，`calc/main.py: set_hook_policy`），此时 `key.install()`
**返回 false 且不碰固件槽位**。默认开是因为机制已经换成 DOOM 那套（真机验证过）；
`make test-input` 里有一条专门验证 `noinputhook` 确实拦得住。

**`key.hookstats()`** 是给真机排错用的 20 个数：armed / slot（槽位里是不是我们的补丁）/
calls / dropped_unarmed / bad_type / 按键按下松开计数 / 触摸 press/move/jitter/release /
`touch_frames` / `touching` + 最近接受的触点坐标 / queue_full / restore_writes /
restore_verified / restore_failed / installs。真机上输入出问题时，这些数字直接说清是哪一层
断的、上次恢复有没有验证过。

**触摸在这台机器上长什么样**（`make test-input` 用注入的触点钉死）：

```lua
key.install()
local ev = key.event()          -- {type="touch", phase=1, x=100, y=50, dx=0, dy=0}
                                -- phase: 1 按下, 2 移动, 8 抬起（固件自己的编号）
local down, x, y = key.touch()  -- 当前有没有手指按着，以及最近一次被接受的位置
```

移动的抖动过滤是 DOOM 的规则：x、y 都与上一次**被接受**的位置相差不超过 1 px 时丢掉
（`hookstats().touch_jitter` 记数），否则带上相对上一次的 `dx/dy` 交付。

#### 文本：5×8 点阵 与 `font` 库（真字体）

两种文字，各有用处：

| | `gfx.text` / `gfx.textbg` | `font` 库（真字体） |
|---|---|---|
| 字形 | hp_gfx 内置 5×8 点阵，640 B | primetcc 引擎 + **Montserrat（比例）/ Cascadia Code Light（等宽）**，**各 16 / 24 / 32 px**；另有钟表用的 **`font.big()` 56 px 数字面**。全部 4-bit 抗锯齿 |
| 占用 | 0 B（已在镜像里） | 引擎 892 B + 两族三个尺寸的字模 ≈ **43 KB**（占镜像 .rodata 的一半）；`font.big()` 另加 6.6 KB |
| 宽度 | 固定 6 px/字符 | 真实宽度（`font.width`），比例字体按字形推进；等宽字体每格同宽 |
| 接口 | `gfx.text(x,y,s,c)`、`gfx.textbg(x,y,s,fg,bg)`、`gfx.textwidth(s)` | `font.prop()/font.mono()/font.cascadia()/font.big()` 取句柄，`font.height/w/handle`、`font.width(h,s)`、`font.advance(h,ch)`、`font.draw(h,x,y,s,c)`、`font.drawbg(h,x,y,s,fg,bg)` |

```lua
local h  = font.prop()                    -- Montserrat，或 font.mono() / font.cascadia()
font.drawbg(h, 8, 40, "Mandelbrot", gfx.WHITE, gfx.BLUE)
local w, lh = font.width(h, "Mandelbrot"), font.height(h)   -- 21 px 行高

local mid = font.mono(32)                 -- 同一族的最大档：行高 38 px，格子 19 px
font.draw(mid, 20, 100, "12:34:56", gfx.CYAN)   -- 152 px 宽

local big = font.big()                    -- 钟表的数字面：行高 66 px，格子 33 px
font.draw(big, 28, 73, "12:34:56", gfx.CYAN)    -- 264 px 宽，占满 320 px 的屏
```

`font.prop(20)` / `font.mono(14)` 这类尺寸参数**取最近的一档**（primetcc 引擎自己的
规则），所以 `prop(20)` 画 24 px、`mono(14)` 画 16 px，`prop(64)` 落到最大的 32 px ——
照着 primetcc 四档写的脚本照样能跑。

#### `font.big()`：钟表用的 56 px 数字面（不是第三套正文字体）

「要大字」和「不能糊」是矛盾的：把 32 px 的面画进 GROB 再放大 3 倍，抗锯齿边缘也
一起放大 —— 每个 1 px 的灰边变成 2~3 px 的灰带，真机上肉眼可见地糊。所以大数字
**按它要显示的尺寸重新栅格化**：

* `port/plua_font_big_data.c`：Cascadia Code Light 原生 56 px，只有
  `'-' '.' '/' '0'-'9' ':'`（0x2D..0x3A）这 14 个字形，**6.6 KB**（全 ASCII 要 ~40 KB）；
  格子 33 px、行高 66 px，`"HH:MM:SS"` 正好 264 px；
* 生成器 `tools/make_font_big.py`（`make font-big` / `make font-big BIGSIZE=64`），
  走的是同一个 `fontgen2.py`，所以 nibble 顺序、行补齐、等宽居中和其他字面完全一致；
* **它只认这 14 个字**：画别的字符**什么都不画**（`hp_fonts.c` 直接跳过，不是画方块）。
  正文字还是用 16/24/32 px 的两族。
* `examples/clock.lua` 用它画时间；`make test-libs` 里有一条专门验证"字母用这个面画
  出来是 0 个像素"，另一条把 `"12:34"` 的 1868 个像素与生成数据逐像素对齐。

**两族是怎么来的（`tools/make_font_montserrat.py` / `make_font_cascadia.py`）**：
primetcc 的 `rt/hp_fonts_data.c` 把 8 档字面（4 尺寸 × 比例/等宽）放在一个翻译单元里，
取字面的 `hp_font_prop_data(i)` 用的是**运行期下标**，链接器没法按需丢弃，一链就是
~70 KB。所以 primeLua 自己生成两份字模文件：

* **Montserrat**：从 primetcc 的表里逐字提取 16/24/32 三档 → `port/plua_font_montserrat_data.c`；
* **Cascadia Code Light**：primetcc 现在提交的那份等宽表里是**别的字体**，所以这份是
  用同一个离线栅格化器（`rt/tools/fontgen2.py` + `CascadiaCode-Light.ttf`）现栅格化出
  16/24/32 三档 → `port/plua_font_cascadia_data.c`。

**primetcc 本体一行没改**（栅格化器是只读调用，输出取完即弃），引擎 `rt/hp_fonts.c`
照原样编译进来，字面访问器（`hp_font_prop_data()` / `hp_font_mono_data()`）由这两份
生成文件提供，所以 `font.mono()` 就是 Cascadia，`font.cascadia()` 是同一个字面的显式
名字。生成脚本可重复执行，输出进仓库，正常构建既不需要 primetcc 的字体数据也不需要
PIL。

**验证**：模拟器里把 `font.draw` 的结果与"从字模数据反推的期望像素"逐像素比对
（含 4-bit alpha 混合的精确值）——16 px 的 `prop`/`mono` 各 104/124 个像素、24 px 的
`prop` 188 个、32 px 的 `mono` 373 个、56 px 的 `big` 1868 个，**全部完全一致**；
`font.drawbg` 的背景方块尺寸、颜色、边界外不着色也都对得上；脚本里还钉死了各档行高
（prop 21/31/40、mono 19/29/38、big 66）、`font.mono()` 与 `font.cascadia()` 在每一档
上指标相同、等宽格的 `":"` 与 `"1"` 推进量相等（钟表数字不会跳）、以及大字体面
画字母 0 像素。指标也对得出 primetcc 注释里的 16 px 值（`"Hello, World!"` = 101 px）。

#### `dd`：31 位十进制的高精度库（新库，不动 `math`）

```lua
local dd = dd
local x = dd(2) * dd.pi()                  -- 2π，31 位
print(x)                                   -- 6.28318530717958647692528676655901e+00
print(dd.sqrt(dd(2)), dd.sin(dd(1)))       -- 都是 33 位
print(dd.tostring(dd.pi(), 20))            -- 也可以只要 20 位
print(dd("3.14159265358979323846264338327950288"))   -- 从长字符串解析
print(dd(9007199254740993) - dd(9007199254740992))   -- 1（double 会给 0）
local hi, lo = dd.parts(dd(1)/dd(3))       -- 精确的两个 double
print(dd(1) < dd(2), dd(3) == dd(3))       -- 比较运算符
print(dd.pi():sqrt())                      -- 方法写法也可以
```

`dd` 是**独立的库**：Lua 的 `math` 与数字类型保持与桌面 Lua 逐位一致（差分测试依赖这一点），
高精度放在自己的命名空间里。技术细节、实测精度表与实现坑（Cody-Waite 归约、dd 不能按
分量除以整数、打印必须两个 double 都满宽输出）都写在 **`port/plua_dd_NOTES.md`**，
主机端对照测试是 `make test-dd`（mpmath 60 位真值，680 个函数用例 + 10 个字符串往返）。

实测（dd-ulp，1 ulp = 2^-106 ≈ 1.08e-32 相对误差）：sin 4.1 / cos 2.2 / tan 3.3 /
asin 4.6 / acos 2.4 / atan 1.5 / atan2 4.3 / **exp 45.6**（|x|>300 时约 30 位）/
log 2.7 / log10 3.7 / sqrt 1.3 / pow 10.0 / hypot 1.2，常数 π/e/ln2/ln10 精确。
参数归约用三字 Cody-Waite，所以 sin/cos 在 |x| 到 1e6 仍保持精度。

已知取舍：`dd("0.1") + dd("0.2") == dd("0.3")` 是 **false**——`dd("0.1")` 是*十进制* 0.1
（比 double 的 0.1 差 5.55e-18，这正是它的意义），两个值各带约 1e-32 舍入，和比 0.3 低
约 2e-32；打印出来能看到 `2.99999999999999999999999999999998e-01`。

#### 真机排错：`hwcheck.lua`（梯子）、`keyprobe.lua`（探针）、`bisect_*.lua`（隔离）

模拟器全绿不代表真机全绿——这一版重建里两个活生生的例子：`gfxdemo` 一装输入钩子
就复位（那时输入层还是 primetcc 的整块 cache/TLB 维护，见 §7 的输入小节；现在是
DOOM 的那套），而模拟器对此**完全无感**。所以有两个**逐级排查脚本**：

* `hwcheck.lua`：把每个有风险的硬件调用一个一个做（get_lcd/整屏 blit/字体/离屏
  GROB/`sys.memory` 大块 malloc 探针/按键钩子/sleep），**每步之前先往 `hwcheck.log`
  写一行**再执行，单步失败用 pcall 接住并继续。真机若复位，日志的最后一行就是凶手；
  没复位则 18 步全绿。
* `keyprobe.lua`：只做输入那一条链，但更细——install / hooked / getkey / sleep /
  event / remove 逐步记录，每一步都打印**并**整体重写 `keyprobe.log`（这台固件的
  文件 API 只可靠支持 `rb`/`wb+`，用 `"a"` 重开写出来的是**空文件**，踩过一次），
  同时把 `key.hookstats()` 的 20 个数写下来。哪一步复位，日志就停在哪一步。
* `bisect_none.lua` / `bisect_clock.lua` / `bisect_time.lua` / `bisect_draw.lua` /
  `bisect_keys.lua` / `bisect_sleep.lua`：**一次只做一个原语，然后正常退出**。
  用法固定：跑完 → 退出 → **摸屏幕** → 输入死没死。哪一步死，哪一步就是元凶。
  `bisect_none`（什么都不做）是**对照组**：如果连它也死，说明机器的脆弱与我们的
  代码无关；`bisect_clock`（把 clock.lua 的键盘部分拿掉）用来回答"输入钩子到底
  有没有关系"。

  历史记录（2026-09-27，那套自创钩子的实测）：

  ```
  bisect_none / bisect_clock / bisect_time / bisect_draw / bisect_sleep  -> 输入正常
  bisect_hookslot.lua   (install+remove，系统从未进入)                   -> 输入正常
  bisect_hookentry.lua  (install，等 3 s，remove；calls=2)               -> 输入死
  ```

  当时的结论是"从我们的代码里过一趟就致命"，于是入口被写成"只拷贝、不调 C"。
  换成 DOOM 的机制后这条结论被推翻：入口现在是普通 C 函数，该解析就解析，
  该调 `svc` 就调 `svc`（§7 的对照表）。**先量，再下结论**——这次的量是
  `puredoom.elf` 本身。

启动器自己也会写一份**持久日志** `plua_launcher.log`（`[selector] picked x`、
`[run] x`、`[exit] x -> 0`、每一条 `[hook] ...` 与 `[selftest] ...`），列表里的
`log` 行会同时显示它和 C 侧的 `plua.log` ✓——真机重启后，两份日志合起来就能定位到
"启动器在做什么、解释器走到哪一步"。

#### 版本号：确认设备上的 lua.elf 是不是新的

`sys.version()`（`PLUA_VERSION`，手写常量，Lua 可见 API 一变就 +1）会由解释器在每次启动时
写进 `plua.log`（`interp: 0.5`），列表的 `log` 行能看到；诊断脚本第一行也会记它。
**换脚本一定要连 `lua.elf` 一起换**——用新的 `.lua` 配旧的 `lua.elf`，症状是
`attempt to call a nil value (field 'target')` 这类"字段不存在"，看着像脚本 bug，
其实是设备上跑着旧解释器。

#### 文件名与模式：固件只认 `"rb"` / `"wb+"`

固件的 `fopen` 只可靠支持 `"rb"` 和 `"wb+"`（primetcc 的 diag 日志就是这么写的，并且
记录了 `"ab"` 不可靠）。Lua 和 C 库说的是普通 C 模式，**原样透传在模拟器里没事，在真机上
`io.open(name, "w")` 直接返回 nil**——诊断脚本的日志文件因此在计算器上一个都没出现。
现在 `port/plua_fwmode.c` 做翻译（`r*`→`rb`；`w*`→`wb+`；`a*`→`wb+` 并把旧内容先写回再
定位到末尾；`r+`→`wb+` 保留内容），`make test-modes` 用 15 条断言把它钉死。

#### 两个"模拟器里看不出来、真机致命"的坑

| 坑 | 症状 | 现在怎么做 |
|---|---|---|
| 帧循环里调 `sys.memory()` | 它是对固件堆的**二分探测**：反复 malloc/free 几百 KB 的大块。primetcc 的注释里记着"大块 alloc/free 探针曾把计算器弄复位"。gfxdemo 原来每帧调一次 → 真机**立即花屏重启** | 只在**任何大块分配之前**调一次，帧循环里彻底不碰 |
| 镜像常驻导致"上一次运行的遗留" | 镜像不重载（`lua.slot`），于是 `hp_gfx.c` 的静态量（当前绘制目标）**跨脚本存活**：上一次运行若把目标留在自己已释放的 GROB 上，下一次 `gfx.clear()` 就写进已回收的内存 | `gfx.init()` 每次重新指向默认后缓冲；`gfx.freegrob()` 释放的正是当前目标时先切回默认；`gfx.blit()` 呈现完还原脚本原来选的目标；新增 `gfx.target()`（`"screen"/"default"/"grob"`），`hp_libs.lua` 逐条钉死 |

**测试**：`make test-libs`（见 §5 第 3b 层）。它抓到过四个真 bug：`fx.todouble`
经 `lua_pushinteger` 返回被截断（`1.5` 变 `1`）、`gfx.blit(grob,x,y)` 强行切到
屏幕而绕过当前目标（离屏合成结果丢帧）、键码表用错（右键退出）、以及绘制目标
跨运行遗留。`main.probe()` 里也加了这些库的探针。

---

## 11. 常见疑问

**为什么 `print(math.sin(1))` 只有 13 位小数，Python 有 16 位？**

不是精度问题，是**打印格式**：Lua 5.4 的 `print`/`tostring` 用 `%.14g`
（`luaconf.h` 的 `LUA_NUMBER_FMT`，桌面 Lua 一样），即 14 位有效数字；Python 的
`repr` 打印"最短可往返"表示。值本身是满精度 double：

```lua
print(string.format("%.17g", math.sin(1)))   --> 0.8414709848078965
print(string.format("%a",    math.sin(1)))   --> 0x1.aed548f090ceep-1
```

**那 primeLua 的 math 到底准不准？** 在模拟器里跑了设备上同一份 `rt_math.o`，788 组
输入、以 mpmath 80 位真值为参考：

| | primeLua | Python（glibc） |
|---|---|---|
| 最大误差 | **1.31 ulp**（tanh） | 1.31 ulp（tanh） |
| 与 Python 逐位相同 | **761/788 = 96.6%** | — |
| 互有胜负 | cosh 0.95 vs 0.83；log10 0.50 vs 0.79 | |

也就是同一档次（openlibm vs glibc，都亚 2-ulp），不存在"不如 Python"。

## 12. 后续可做

1. **计算器端编辑器**：列表目前只选文件；加一个行编辑器就能在机器上直接写脚本
   （键码表已经有了，字母键的设备键码见 `HP_Prime_GETKEY.md`）。
2. **`ti` 库（TI-Nspire 兼容层）**：这一版先不做，源码在 `../_attic/primeLua-ti/`
   （放回去只要一条 `mv` 加 Makefile 里一行 `cp`，见那边的 README）。
3. ~~**输出流式化**~~：**已做**（1.0）——解释器跑在固件线程上、启动器边跑边读，
   并且 ESC/ON 能中断（§7.5，`make test-stream`）。
4. **`LUA_32BITS` 构建变体**：内存更省、软浮点更快，代价是精度降为 float。
5. **多点触摸（整帧接触表）**：`key.gettouch`/`key.event`/`key.touch` 都只看**一个**
   触点（固件帧里的第一项），DOOM 的抖动过滤也在。把整帧触点表暴露给 Lua
   （`key.contacts()`、`key.touchframes()`）是 primeLua 927 那条通路，代码和它的测试
   一起放在 `../_attic/primeLua-touch/`，放回去的步骤见那边的 README —— 这一版不带，
   因为它当年"在真机上表现不稳"的原因（就是那套自创的钩子写法）现在已经换掉了，
   想重做的话在 DOOM 的机制上重做，别把旧代码搬回来。
6. **运行时的输入**：现在能中断，但控制台本身不吃输入。真正的交互式终端要 Python 侧
   和程序同时读写屏幕（一个行编辑器就是第一步，键码表已经有了）。
