--# no-compare
--# expect-exit: 0
--# max-insn: 8000000000
-- mandel.lua -- the Mandelbrot set, drawn with the gfx library.
--
-- This used to print the set as ASCII art through the launcher's console.  Now
-- it draws it on the calculator's own screen: gfx.init() starts with the draw
-- target on the SCREEN, so the picture appears row by row as it is computed --
-- the progress display and the result are the same thing, and no 307 KB back
-- buffer or full-screen blit is needed (the two operations that were resetting
-- the box on real hardware).
--
-- WHY IT IS COMPUTED IN BLOCKS: this machine has no FPU, so every iteration is
-- soft-float double work.  SCALE=2 computes one point per 2x2 block (160x120
-- points) and fills the whole block: a quarter of the arithmetic for a picture
-- that still looks smooth at 320x240.
--     SCALE = 1   sharp,  ~4x slower   (320x240 points)
--     SCALE = 2   default              (160x120 points)
--     SCALE = 4   fast,   ~4x quicker  (80x60 points, visibly blocky)
--
-- The launcher is blocked inside one synchronous debug call for the whole run
-- (that is how a program runs in this port), so the work is bounded by MAXITER
-- and the window below.  HOLD_MS keeps the finished picture on screen for a
-- moment before the launcher prints its summary and takes the display back
-- (set it to 0 to skip).

local gfx, sys = gfx, sys

-- real text when the interpreter has the font library (16 px proportional),
-- the 5x8 bitmap font otherwise -- so the script still runs on an older
-- lua.elf, just less pretty
local face = (type(font) == "table" and font.prop) and font.prop() or nil

local function label(x, y, s, c, bg)
  if face then
    font.drawbg(face, x, y, s, c, bg)
  else
    gfx.textbg(x, y, s, c, bg)
  end
end

local SCALE = 2
local W, H = 320, 240
local MAXITER = 32
local X0, X1 = -2.2, 0.8            -- the classic window
local Y0, Y1 = -1.125, 1.125        -- 4:3, so the pixels come out square
local HOLD_MS = 2500

local CW, CH = W // SCALE, H // SCALE

-- escape speed -> colour; the inside of the set keeps the background
local cols = { gfx.BLUE, gfx.CYAN, gfx.GREEN, gfx.YELLOW, gfx.ORANGE, gfx.RED,
               gfx.MAGENTA, gfx.WHITE }
local NCOL = #cols

local function colour(i)
  local k = (i * NCOL) // MAXITER + 1
  if k < 1 then
    k = 1
  elseif k > NCOL then
    k = NCOL
  end
  return cols[k]
end

local function log(msg)
  if sys and sys.log then
    sys.log("mandel: " .. msg)
  end
end

if not gfx.init() then
  print("mandel: no display")
  return
end

gfx.clear(gfx.BLACK)
log(string.format("start %dx%d points, %dx%d blocks, iter<=%d",
                  CW, CH, SCALE, SCALE, MAXITER))

local escaped = 0
for py = 0, CH - 1 do
  local ci = Y0 + (Y1 - Y0) * py / (CH - 1)
  local sy = py * SCALE
  for px = 0, CW - 1 do
    local cr = X0 + (X1 - X0) * px / (CW - 1)
    local zr, zi, i = 0.0, 0.0, 0
    local zr2, zi2 = 0.0, 0.0
    while i < MAXITER do
      zr2 = zr * zr
      zi2 = zi * zi
      if zr2 + zi2 > 4.0 then
        break
      end
      zi = 2.0 * zr * zi + ci
      zr = zr2 - zi2 + cr
      i = i + 1
    end
    if i < MAXITER then             -- escaped: colour by how fast
      escaped = escaped + 1
      gfx.fillrect(px * SCALE, sy, SCALE, SCALE, colour(i))
    end
  end
  if py % 8 == 0 or py == CH - 1 then
    local done = py + 1
    gfx.hline(0, H - 1, (done * W) // CH, gfx.GRAY)     -- progress bar
    label(4, 2, string.format("mandelbrot  %d%%   %dx%d  iter<=%d",
                              (done * 100) // CH, CW, CH, MAXITER),
          gfx.WHITE, gfx.BLACK)
  end
end

local lh = face and font.height(face) or 8
label(4, H - 2 * lh - 4, string.format("%d of %d points escaped (%.1f%%)",
                                       escaped, CW * CH,
                                       100.0 * escaped / (CW * CH)),
      gfx.WHITE, gfx.BLACK)
label(4, H - lh - 2, "SCALE=" .. SCALE .. "  MAXITER=" .. MAXITER ..
      "  -- edit mandel.lua to change", gfx.GRAY, gfx.BLACK)
log(string.format("done: %d of %d points escaped", escaped, CW * CH))

-- let the finished picture be looked at: the launcher prints its summary (and
-- the terminal takes the display back) the moment this script returns
if HOLD_MS > 0 and sys and sys.sleep then
  pcall(sys.sleep, HOLD_MS)
end

print(string.format("mandelbrot drawn: %dx%d points as %dx%d blocks, "
                    .. "%d escaped, iter<=%d", CW, CH, SCALE, SCALE, escaped,
                    MAXITER))
