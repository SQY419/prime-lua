--# no-compare
--# expect-exit: 0
--# max-insn: 400000000
-- hwcheck.lua -- find out WHICH hardware call reboots the calculator.
--
-- gfxdemo drew a garbled screen and reset the machine on real hardware, while
-- the same script runs clean in the emulator.  Guessing from here is useless,
-- so this script walks the risky calls ONE AT A TIME and writes a line to
-- hwcheck.log before each one.  A reset leaves the log ending on the guilty
-- call; a failure is caught with pcall and reported instead of crashing.
--
-- Every step is written to BOTH places, because either one can be the only
-- survivor:
--     * sys.log()  -> plua.log, the interpreter's own durable log.  On the
--       calculator the launcher's "log" row shows it, and it survives a reset.
--       This is the one to look at first.
--     * a log file -> hwcheck.log, for reading on a PC.
-- If the machine resets, the LAST line written names the call that did it.
--
-- What each step would tell us:
--   gfx.init        get_lcd (svc 0x1008D) + a 307 KB GROB malloc
--   clear/blit      writes the whole 320x240 framebuffer
--   text/pixel      the font blitter and the checked pixel store
--   small grob      a second GROB, then draw into it off-screen
--   sys.memory      the big malloc/free probe (primetcc warns this can reset)
--   key.install     patches the firmware input slot
--   sleep           svc 0x10008 from inside the debug call
--   blitpixels      string.pack -> a raw rectangle blit

local gfx, key, sys = gfx, key, sys

local LOG = "hwcheck.log"
local logf = io.open(LOG, "w")
local pass, fail = 0, 0
local stopped = false

local function mark(what)
  if sys.log then
    sys.log("hwcheck: " .. what)      -- durable: plua.log, survives a reboot
  end
  if logf then
    logf:write(what .. "\n")
    logf:flush()
  end
end

if not sys.log then
  print("note: this lua.elf has no sys.log(); using the file only")
end
if not sys.version then
  print("note: this lua.elf is OLDER than the scripts (no sys.version) -- " ..
        "copy the new lua.elf to the calculator")
end

-- run one step: log it first, catch errors.  After the first failure the
-- ladder STOPS: continuing past a broken state tells us nothing and multiplies
-- the ways the calculator can die.
local function step(name, fn)
  if stopped then
    return
  end
  mark("start  " .. name)
  local ok, err = pcall(fn)
  if ok then
    pass = pass + 1
    mark("ok     " .. name)
    print(string.format("%-22s ok", name))
  else
    fail = fail + 1
    stopped = true
    mark("FAIL   " .. name .. ": " .. tostring(err))
    print(string.format("%-22s FAIL %s", name, tostring(err)))
  end
end

mark("---- hwcheck start ----")
mark("interpreter " .. (sys.version and sys.version() or "?") ..
     ", PPL/emulator build differs -- this line is what to compare")

step("gfx.init", function()
  assert(gfx.init(), "gfx.init returned false")
  assert(gfx.width() == 320 and gfx.height() == 240,
         string.format("unexpected screen %dx%d", gfx.width(), gfx.height()))
end)
step("gfx.target after init", function()
  -- drawing starts on the SCREEN: no 307 KB back buffer until a script asks
  -- for one with gfx.select()
  assert(gfx.target() == "screen", "target is " .. tostring(gfx.target()))
end)
step("gfx.fb (firmware's LCD struct)", function()
  if not gfx.fb then
    mark("  this lua.elf has no gfx.fb()")
    return
  end
  local fb, w, h = gfx.fb()
  mark(string.format("  framebuffer 0x%08X %dx%d (gfx says %dx%d)", fb, w, h,
                     gfx.width(), gfx.height()))
  assert(fb ~= 0, "the firmware named no framebuffer")
  assert(w == gfx.width() and h == gfx.height(),
         string.format("LCD struct says %dx%d, gfx says %dx%d", w, h,
                       gfx.width(), gfx.height()))
end)
step("gfx.clear", function() gfx.clear(gfx.BLACK) end)
step("gfx.fillrect", function()
  gfx.fillrect(10, 10, 100, 40, gfx.RED)
end)
step("gfx.line/circle", function()
  gfx.line(0, 0, 319, 239, gfx.WHITE)
  gfx.circle(160, 120, 30, gfx.YELLOW)
end)
step("gfx.text", function()
  gfx.text(4, 4, "hwcheck", gfx.WHITE)
  assert(gfx.textwidth("hwcheck") > 0, "textwidth returned 0")
end)
step("gfx.getpixel readback", function()
  local c = gfx.getpixel(15, 25)
  assert(c == gfx.RED, string.format("read back 0x%X", c))
end)
step("gfx.blit (whole screen)", function() gfx.blit() end)

-- the buffered pattern, on its own rung: allocate the 307 KB back buffer,
-- draw into it, then present it in one copy (this is what gfxdemo used to do
-- every frame, and both halves are separately interesting)
step("back buffer + full blit", function()
  gfx.select()
  assert(gfx.target() == "default")
  gfx.clear(gfx.BLUE)
  gfx.fillrect(20, 20, 60, 60, gfx.GREEN)
  mark("  allocated the back buffer, presenting it now")
  gfx.blit()
  mark("  presented the back buffer")
  gfx.select(false)
  assert(gfx.target() == "screen")
end)

step("newgrob(64,48)", function()
  local g = gfx.newgrob(64, 48)
  assert(g, "newgrob returned nothing")
  -- the previous rung left the target on the screen; making a GROB must not
  -- move it
  assert(gfx.target() == "screen", "target changed by newgrob")
  gfx.select(g)
  mark("  drawing into the grob")
  gfx.clear(gfx.MAGENTA)
  gfx.select()
  mark("  grob -> back buffer")
  gfx.blit(g, 240, 160)
  gfx.blit()
  mark("  freeing the grob")
  gfx.freegrob(g)
  assert(gfx.target() == "default", "target is " .. tostring(gfx.target()))
end)

step("blitpixels (raw rect)", function()
  -- "<I4" packs ONE integer: extra arguments are ignored (Lua 5.4), so the
  -- format has to repeat -- #string.pack("<I4",a,b,c,d) is 4, not 16
  local px = string.pack(string.rep("I4", 4), gfx.BLUE, gfx.BLUE, gfx.BLUE,
                         gfx.BLUE)
  assert(#px == 16, "packed " .. #px .. " bytes")
  gfx.blitpixels(px, 4, 1, 8, 200)
  gfx.blit()
end)

step("sys.memory (big malloc probe)", function()
  local before = sys.memory()
  mark(string.format("  sys.memory -> %d KB", before // 1024))
end)
step("sys.maxalloc", function()
  local m = sys.maxalloc()
  mark(string.format("  sys.maxalloc -> %d KB", m // 1024))
end)

step("key.install", function()
  assert(key.install(), "key.install refused")
  assert(key.hooked(), "not hooked after install")
end)
step("key.getkey (no wait)", function()
  local k = key.getkey()
  assert(k == nil or type(k) == "number", "odd key value")
end)
step("sys.sleep(20)", function() sys.sleep(20) end)
step("key.remove", function()
  key.remove()
  assert(not key.hooked(), "still hooked after remove")
end)

mark(string.format("---- hwcheck end: %d ok, %d FAIL%s ----", pass, fail,
                   stopped and ", stopped at the first failure" or ""))
if logf then
  logf:close()
end

-- leave something on screen that says how it went (drawing straight to the
-- screen here: no blit, so this works even if the buffered path was the rung
-- that failed)
gfx.select(false)
gfx.fillrect(0, 0, 320, 30, fail == 0 and gfx.GREEN or gfx.RED)
gfx.text(4, 8, string.format("hwcheck: %d ok, %d FAIL", pass, fail), gfx.BLACK)
gfx.text(4, 60, "logs: plua.log + hwcheck.log", gfx.WHITE)

print(string.format("hwcheck: %d ok, %d FAIL -- see plua.log (log row) and %s",
                    pass, fail, LOG))
