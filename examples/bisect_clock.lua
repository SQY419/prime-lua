--# no-compare
--# expect-exit: 0
--# max-insn: 900000000
-- bisect_clock.lua -- ONE primitive, then exit.  what clock.lua does, MINUS the keyboard: gfx + os.time for ~20 s.
--
-- HOW TO USE (same for every bisect_*.lua):
--   1. pick it in the list, let it finish (it prints as it goes)
--   2. leave the program (any key / ESC)
--   3. TOUCH THE SCREEN
--   4. keys+ touch dead?  -> this step is the culprit, tell me the file name.
--      still fine?         -> this primitive is exonerated.
--   5. reboot before the next step when a step did kill input.
--
-- WHY THESE EXIST: after a program that uses the keyboard exits, touching the
-- screen on this machine can leave keys and touch dead, and the calculator
-- resets.  The input hook is provably clean by then -- the launcher log shows
-- after every run:
--
--     [hook] stand-down: armed=0 (state 0x...)
--     [hook] slot now: E92D0001 E92D4000 (firmware stub)
--
-- so the damage is done by something the program does WHILE it runs.  Each of
-- these scripts does exactly one of the candidates, for the same wall time.
--
-- bisect_none.lua   nothing at all -- THE CONTROL. If the machine dies after
--                   this one too, the fragility has nothing to do with us.
-- bisect_clock.lua  what clock.lua does, minus the keyboard: gfx + os.time in
--                   a loop for ~20 s.  The most direct test of "is the input
--                   hook involved at all".
-- bisect_time.lua   os.time() in a loop (svc 0x100A5, the firmware clock).
-- bisect_draw.lua   gfx only: full-screen fill + text, ~600 frames.
-- bisect_keys.lua   the input hook only: install, poll getkey(200) x 150,
--                   remove.
-- bisect_sleep.lua  sys.sleep(20) x 600 (already reported clean -- kept so the
--                   matrix is complete).

local LOG = "bisect_clock.log"

-- The log is kept in memory and rewritten with "w" on every mark: this
-- firmware only supports "rb"/"wb+" reliably (port/plua_fwmode.c), and the
-- earlier "a"-mode version wrote an EMPTY file on the device.  Everything is
-- printed as well, so the launcher's console shows the same trail.
local lines = {}
local function mark(what)
  lines[#lines + 1] = what
  local f = io.open(LOG, "w")
  if f then
    f:write(table.concat(lines, "\n") .. "\n")
    f:flush()
    f:close()
  end
  print(what)
  if sys.log then sys.log("clock: " .. what) end
end

mark("---- bisect_clock.lua ----")
mark("version " .. tostring(sys.version and sys.version() or "?"))
local t0 = os.time()

-- clock.lua with key.install()/getkey()/key.remove() taken out: if input dies
-- after THIS one, the input hook is not the cause, whatever else is.
local gfx, sys = gfx, sys
local font = font
if gfx.init then gfx.init() end
gfx.select(false)
local W, H = gfx.width(), gfx.height()
local face = (font.big and font.big()) or font.cascadia(32)
local cell = font.advance(face, "0")
local lh = font.height(face)
mark(string.format("gfx %dx%d, cell %d, line %d", W, H, cell, lh))

local TICKS, TICK_MS = 100, 200          -- ~20 s
local prev = nil
for tick = 1, TICKS do
  local t = os.time()
  local s = os.date("%H:%M:%S", t)
  local date = os.date("%Y-%m-%d  %A", t)
  if s ~= prev then
    local x0 = math.floor((W - #s * cell) / 2)
    for i = 1, #s do
      local ch = s:sub(i, i)
      if not prev or prev:sub(i, i) ~= ch then
        local cx = x0 + (i - 1) * cell
        gfx.fillrect(cx, 80, cell, lh, gfx.BLACK)
        font.draw(face, cx, 80, ch, gfx.CYAN)
      end
    end
    gfx.textbg(4, 4, date, gfx.WHITE, gfx.BLACK)
    gfx.textbg(4, H - 14, "bisect: clock WITHOUT the key hook", gfx.GRAY, gfx.BLACK)
    prev = s
  end
  sys.sleep(TICK_MS)
end
mark("drew " .. TICKS .. " clock ticks in " .. (TICKS * TICK_MS / 1000) .. " s, no key API used")


mark(string.format("---- bisect_clock.lua done, %d s ----", os.time() - (t0 or 0)))
print("bisect_clock.lua: done -- NOW LEAVE THE PROGRAM AND TOUCH THE SCREEN")
