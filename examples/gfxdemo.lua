--# no-compare
--# expect-exit: 0
--# max-insn: 600000000
-- gfxdemo.lua -- the hardware libraries, on the calculator
--
-- Run it from the launcher list (or `main.run_file("gfxdemo.lua")`).
--
--   arrows        move the box
--   ENTER         grow it
--   BACKSPACE     shrink it
--   ESC           quit
--
-- The loop is BOUNDED (MAX_FRAMES below, a few seconds): it always hands the
-- terminal back, even if a key never arrives.  While a Lua program runs the
-- Python side is inside one blocking debug call, so an endless loop would
-- leave the calculator with no way out but a reset.
--
-- Three things this script is careful about, all learned on the real machine:
--
--   * keys only arrive through the key hook, which this script installs and
--     removes again on the way out (a left-behind hook makes the next program
--     that uses the keyboard crash);
--   * scan codes come from the library, never from a literal: key.getkey()
--     returns the firmware's DEVICE KEY IDS (esc 1, left 2, up 3, right 4,
--     down 5, backspace 0x0C, enter 0x0D), which are NOT the PPL GETKEY
--     numbers (esc 4, left 7, up 2, right 8, down 12, backspace 19, enter 30).
--     Writing the GETKEY numbers here once made RIGHT quit the program;
--   * NO sys.memory() IN THE FRAME LOOP.  That call binary-searches the
--     largest block the firmware heap will hand out, i.e. it mallocs and frees
--     multi-hundred-KB blocks ~20 times -- and primetcc's own notes record a
--     large alloc/free probe REBOOTING the calculator.  It is asked once,
--     before anything is allocated, and the frame loop stays free of it;
--   * NO FULL-SCREEN BACK BUFFER either (gfx.init() makes one when the script
--     does not ask otherwise).  The demo drawing straight to the screen costs
--     a little flicker and saves a 307 KB malloc plus a 307 KB blit per frame,
--     which are exactly the two operations that were rebooting the box on the
--     real hardware while running fine in the emulator.  The ladder script
--     (hwcheck.lua) exercises both, one at a time, with logging.
--
-- Every risky step is written to a small log file in the app folder BEFORE it
-- runs, so if the calculator does reset, the last line names the call that did
-- it (read it on a PC, or with the launcher's "log" row).  The file is opened
-- once in "w" mode -- the firmware only accepts the modes the launcher already
-- uses -- and every write goes straight through, so nothing is lost in a
-- buffer when the machine goes down.

local gfx, key, sys = gfx, key, sys

local K_UP, K_DOWN = key.UP, key.DOWN
local K_LEFT, K_RIGHT = key.LEFT, key.RIGHT
local K_ESC, K_BACKSPACE, K_ENTER = key.ESC, key.BACKSPACE, key.ENTER

local MAX_FRAMES = 400                   -- ~8 s at 20 ms a frame
local LOG = "gfxstep.log"

-- One handle for the whole run, written before each risky call.
local logf = io.open(LOG, "w")
local function mark(what)
  if logf then
    logf:write(what .. "\n")
    logf:flush()
  end
end

mark("---- gfxdemo start ----")

-- the one and only look at free memory: before any large allocation
local free_kb = -1
local ok_mem, mem = pcall(sys.memory)
if ok_mem and type(mem) == "number" then free_kb = mem // 1024 end

-- log through the interpreter when it supports it (durable, no file handling
-- needed), and fall back to a plain file for an older lua.elf
local function logmark(what)
  mark(what)
  if sys.log then
    sys.log("gfxdemo: " .. what)
  end
end

logmark("gfx.init")
gfx.init()
-- draw straight to the screen: no 307 KB back buffer, no 307 KB blit
gfx.select(false)
logmark(string.format("gfx.init done: %dx%d, target=%s, free=%d KB",
                      gfx.width(), gfx.height(),
                      gfx.target and gfx.target() or "?", free_kb))

local W, H = gfx.width(), gfx.height()
local size = 40
local x, y = (W - size) / 2, (H - size) / 2
local frames, quit = 0, false

-- one frame: erase, draw, present
local function frame()
  gfx.clear(gfx.BLACK)
  gfx.fillrect(x, y, size, size, gfx.CYAN)
  gfx.rect(x - 1, y - 1, size + 2, size + 2, gfx.WHITE)
  gfx.text(4, 4, string.format("x=%3d y=%3d size=%3d", x, y, size), gfx.YELLOW)
  gfx.text(4, 16, string.format("frame %d/%d", frames, MAX_FRAMES), gfx.GRAY)
  gfx.text(4, H - 12, "arrows move, ENTER bigger, BACKSPACE smaller, ESC out",
           gfx.GRAY)
end

logmark("first frame")
frame()
logmark("first frame drawn")

logmark("key.install")
key.install()
logmark("key hook installed, hooked=" .. tostring(key.hooked()))

print("gfxdemo: interpreter " ..
      (sys.version and sys.version() or "?") ..
      ", screen " .. W .. "x" .. H .. ", free memory " .. free_kb .. " KB")

while not quit and frames < MAX_FRAMES do
  frames = frames + 1
  frame()

  -- drain every key that arrived since the last frame.  key.getkey() returns
  -- NIL when nothing is pending (and skips key releases), so the loop is
  -- written as `if not kc then break end` -- and bounded anyway: an
  -- unbounded drain would lock the calculator if a code ever repeated.
  for _ = 1, 16 do
    local kc = key.getkey()
    if not kc then break end
    if kc == K_ESC then quit = true
    elseif kc == K_LEFT then x = math.max(0, x - 4)
    elseif kc == K_RIGHT then x = math.min(W - size, x + 4)
    elseif kc == K_UP then y = math.max(0, y - 4)
    elseif kc == K_DOWN then y = math.min(H - size, y + 4)
    elseif kc == K_ENTER then size = math.min(120, size + 4)
    elseif kc == K_BACKSPACE then size = math.max(8, size - 4)
    end
  end

  if frames == 1 then logmark("sleep 20 (first frame)") end
  sys.sleep(20)                          -- ~50 frames a second
end

logmark("key.remove")
key.remove()
gfx.clear(gfx.BLACK)
gfx.text(4, 4, quit and "gfxdemo: bye" or "gfxdemo: frame limit reached",
         gfx.GREEN)
logmark(string.format("---- gfxdemo end: %d frame(s), %s ----", frames,
                      quit and "ESC" or "frame limit"))

if logf then
  logf:close()
end
print(string.format("gfxdemo: %d frame(s), %s; step log in %s", frames,
                    quit and "ended on ESC" or "hit the frame limit", LOG))
