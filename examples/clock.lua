--# no-compare
--# expect-exit: 0
--# max-insn: 900000000
-- clock.lua -- the calculator as a wall clock.
--
--   big line    HH:MM:SS in Cascadia Code Light at 56 px (font.big())
--   small line  YYYY-MM-DD + weekday in the built-in 5x8 bitmap font
--   ESC         quit (any other key is ignored: the clock keeps running)
--
-- Run it from the launcher list, or `main.run_file("clock.lua")`.
--
-- THE TIME IS THE CALCULATOR'S OWN: os.time()/os.date() read the firmware clock
-- (svc 0x100A5 -- see calc/timeprobe.py and README section 4.4) and fall back to
-- the epoch the launcher read from PPL.  With neither source the big line shows
-- --:--:-- and the status line says so, instead of pretending it is 1970;
-- sys.time() is what tells the two apart.
--
-- WHY THE BIG DIGITS ARE A FACE OF THEIR OWN (font.big()): they have to be big
-- AND sharp, and those two pull in opposite directions.  The digits must not
-- move either, so the face needs a fixed cell (":" advancing exactly like "1").
--
--   * a face scaled up is a blurry face: the first version rendered the 16 px
--     glyphs into a GROB and blitted them back x3, so every 1 px antialiased
--     fringe became a 3 px grey band.  Magnifying a bitmap magnifies its soft
--     edges too -- on the calculator that is very visible.
--   * the 32 px text face (font.cascadia(32)) is crisp but small: 8 cells of
--     19 px are 152 px of a 320 px screen wide.
--
-- port/plua_font_big_data.c therefore holds Cascadia rasterized NATIVELY at
-- 56 px, and only the glyphs a clock needs ('-' '.' '/' '0'-'9' ':'), which
-- keeps it at ~6.6 KB.  Cell 33 px, line height 66 px: "HH:MM:SS" is 264 px
-- wide -- it fills the screen, and the edges are as clean as the small faces'.
--
-- The face carries nothing else: a letter drawn with it shows NOTHING (that is
-- why the date line stays in the 5x8 font).
--
-- If font.big() is missing (an older lua.elf) the script falls back to
-- font.cascadia(32), then font.mono(32), then Montserrat, and the top-left line
-- names what it got.
--
-- Three rules this script follows, all learned on the real machine (gfxdemo.lua
-- documents the same ones):
--
--   * drawing goes STRAIGHT TO THE SCREEN (gfx.select(false)): no 307 KB back
--     buffer and no full-screen blit -- exactly the two operations that were
--     rebooting the calculator on real hardware while running fine in the
--     emulator.  Only the characters that changed are repainted (a cell-sized
--     fillrect plus one glyph);
--   * the key hook is installed and REMOVED again on the way out (a hook left
--     behind makes the next program that uses the keyboard crash);
--   * the loop is BOUNDED (MAX_TICKS below, ~5 minutes): the Python side sits
--     inside one blocking call for as long as a script runs, so an endless loop
--     would leave no way out but a reset.  ESC leaves at any time; raise
--     MAX_TICKS for a longer run.
--
-- Risky steps are logged BEFORE they run (clock.log, plus plua.log through
-- sys.log), so a reset leaves the name of the call that did it -- hwcheck.lua's
-- protocol.

local gfx, key, sys = gfx, key, sys
local font = font

local TICK_MS = 200              -- key poll: a new second appears within 200 ms
local MAX_TICKS = 1500           -- ~5 minutes, then the terminal comes back
local COLOR_TIME = gfx.CYAN      -- the big digits are drawn in this colour
local COLOR_DATE = gfx.WHITE
local COLOR_NOTE = gfx.GRAY
local LOG = "clock.log"

local logf = io.open(LOG, "w")
local function mark(what)
  if logf then
    logf:write(what .. "\n")
    logf:flush()
  end
end

local function logmark(what)
  mark(what)
  if sys.log then
    sys.log("clock: " .. what)
  end
end

logmark("---- clock start ----")

-- ---------------------------------------------------------------------------
-- screen
-- ---------------------------------------------------------------------------
logmark("gfx.init")
local ok_init = pcall(function()
  assert(gfx.init(), "gfx.init returned false")
end)
if not ok_init then
  print("clock: no display (gfx.init failed) -- nothing to do")
  return
end
gfx.select(false)                -- straight to the screen: no back buffer
local W, H = gfx.width(), gfx.height()
logmark(string.format("gfx.init ok: %dx%d, target=%s", W, H, gfx.target()))

-- ---------------------------------------------------------------------------
-- fonts: the native big digit face (font.big(), Cascadia at 56 px -- 33 px per
-- cell, so "HH:MM:SS" is 264 px of the 320 px screen), with fallbacks so this
-- script still runs on an older lua.elf
-- ---------------------------------------------------------------------------
local BIG_SIZE = 32               -- only used by the fallbacks below

local function pick_face()
  if font.big then
    return font.big(), "Cascadia Code Light 56px (native)"
  end
  if font.cascadia then
    return font.cascadia(BIG_SIZE), "Cascadia Code Light 32px (no big face)"
  end
  if font.mono then
    return font.mono(BIG_SIZE), "Cascadia Code Light 32px (via font.mono)"
  end
  return font.prop(BIG_SIZE), "Montserrat 32px (no monospace face)"
end

local big, face_name = pick_face()
local cell = font.advance(big, "0")
local line_h = font.height(big)
if not cell or cell <= 0 then
  cell = 16
end
logmark(string.format("big face: %s, cell %d px, line height %d px",
                      face_name, cell, line_h))

-- ---------------------------------------------------------------------------
-- layout: the time line centred, the date under it, notes at the edges
-- ---------------------------------------------------------------------------
local time_y = math.floor((H - line_h) / 2) - 14
local date_y = time_y + line_h + 12
local note_y = H - 22

local function centre_x(width)
  return math.max(0, math.floor((W - width) / 2))
end

-- ---------------------------------------------------------------------------
-- what to show
-- ---------------------------------------------------------------------------
local WEEKDAY = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                  "Friday", "Saturday" }

local function read_clock()
  local t = os.time()
  if not t or t == 0 then
    return "--:--:--", "(no clock source: showing nothing rather than 1970)",
           false
  end
  local d = os.date("*t", t)
  local b = string.format("%02d:%02d:%02d", d.hour, d.min, d.sec)
  -- os.date("*t").wday is Lua's 1-7 with 1 = Sunday; the names are English
  -- because every face here only has ASCII 0x20-0x7E (no Chinese glyphs)
  local wd = WEEKDAY[d.wday] or os.date("%A", t)
  local small = string.format("%04d-%02d-%02d  %s", d.year, d.month, d.day, wd)
  return b, small, true
end

local function describe_source()
  if sys.time and sys.time() then
    return "firmware clock (svc 0x100A5)"
  end
  return "launcher epoch (no firmware clock on this machine)"
end

-- repaint ONLY the characters that changed: no flicker on the digits that
-- stayed the same, and a refresh costs one cell per changed digit
local function draw_time(s, prev)
  local x0 = centre_x(#s * cell)
  for i = 1, #s do
    local ch = s:sub(i, i)
    if not prev or prev:sub(i, i) ~= ch then
      local cx = x0 + (i - 1) * cell
      gfx.fillrect(cx, time_y, cell, line_h, gfx.BLACK)
      font.draw(big, cx, time_y, ch, COLOR_TIME)
    end
  end
end

local function draw_date(s)
  gfx.textbg(centre_x(gfx.textwidth(s)), date_y, s, COLOR_DATE, gfx.BLACK)
end

-- ---------------------------------------------------------------------------
-- first paint
-- ---------------------------------------------------------------------------
logmark("first paint")
gfx.clear(gfx.BLACK)

local now_big, small, have_clock = read_clock()
draw_time(now_big, nil)
draw_date(small)
gfx.textbg(2, 2, "clock.lua  " .. face_name, COLOR_NOTE, gfx.BLACK)
gfx.textbg(2, note_y, "ESC = exit   " .. describe_source(), COLOR_NOTE, gfx.BLACK)
logmark("first paint done: " .. now_big .. "  " .. small)

print(string.format("clock: %s, %s, %s -- ESC to quit, limit %d s",
                    face_name, describe_source(),
                    have_clock and "clock running" or "NO CLOCK SOURCE",
                    (MAX_TICKS * TICK_MS) // 1000))

logmark("key.install")
key.install()
logmark("key hook installed, hooked=" .. tostring(key.hooked()))

-- ---------------------------------------------------------------------------
-- the loop: one tick per TICK_MS, redraw only what changed
-- ---------------------------------------------------------------------------
local ticks, quit = 0, false
local prev_big = now_big
local K_ESC = key.ESC

while not quit and ticks < MAX_TICKS do
  ticks = ticks + 1

  -- key.getkey(wait) is both the wait and the poll: no busy loop, ESC answers
  -- within TICK_MS, and nil means "nothing was pressed"
  if key.getkey(TICK_MS) == K_ESC then
    quit = true
  end

  if not quit then
    local nb, ns = read_clock()
    if nb ~= prev_big then
      draw_time(nb, prev_big)
      prev_big = nb
      if ns ~= small then
        small = ns
        draw_date(small)
      end
    end
  end
end

-- ---------------------------------------------------------------------------
-- out
-- ---------------------------------------------------------------------------
logmark("key.remove")
key.remove()

local secs = (ticks * TICK_MS) // 1000
gfx.clear(gfx.BLACK)
gfx.textbg(4, 4, quit and "clock: bye" or
           string.format("clock: %d s limit reached", secs),
           gfx.GREEN, gfx.BLACK)
gfx.textbg(4, 20, "ESC / ON returns to the launcher", COLOR_NOTE, gfx.BLACK)
logmark(string.format("---- clock end: %d tick(s) (%d s), %s ----",
                      ticks, secs, quit and "ESC" or "tick limit"))

if logf then
  logf:close()
end

print(string.format("clock: %d tick(s), %s; step log in %s", ticks,
                    quit and "ended on ESC" or "hit the time limit", LOG))
