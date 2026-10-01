--# no-compare
--# expect-exit: 0
--# max-insn: 400000000
-- imgdemo.lua -- the img library: load a JPG/PNG, then blit and scale it.
--
--   img.load(path)              -> a GROB (or nil, error message)
--   img.info(path)              -> w, h, "png" | "jpg"
--   img.blit(g, x, y [, key])   1:1 copy onto the current target
--   img.strblit(g, x, y, w, h [, key])          scaled copy
--   img.blitpart(g, dx, dy, sx, sy, sw, sh [, key])
--   img.strblitpart(g, dx, dy, dw, dh, sx, sy, sw, sh [, key])
--   img.scale(g, w, h)          -> a new, resized GROB
--
-- The loaded image IS an ordinary GROB, so gfx.select/gfx.blit/gfx.grobwidth/
-- gfx.freegrob work on it with no special cases.
--
-- WHAT IT LOOKS FOR: this demo tries a few file names in the app folder
-- ("imgdemo.jpg", "imgdemo.png", then a few common ones).  Drop your own JPEG or
-- PNG in the folder under one of those names to see it.  With no image at all it
-- builds one in memory and still exercises every call, so it is never a blank
-- screen and the harness can run it with no fixture.
--
-- Press ESC to leave (the loop is bounded, ~20 s, so the terminal always comes
-- back even if the keyboard is quiet).

local gfx, key, sys, img = gfx, key, sys, img

local CANDIDATES = { "imgdemo.jpg", "imgdemo.png", "bg.jpg", "bg.png",
                     "photo.jpg", "photo.png", "test.png" }
local MAX_TICKS = 1000
local TICK_MS = 20

local function mark(s)
  if sys.log then sys.log("imgdemo: " .. s) end
end

if not gfx.init() then
  print("imgdemo: no display")
  return
end
gfx.select(false)
local W, H = gfx.width(), gfx.height()

-- ---------------------------------------------------------------------------
-- find an image, or make one
-- ---------------------------------------------------------------------------
local g, name, words, kind = nil, nil, nil, nil

for _, cand in ipairs(CANDIDATES) do
  local w, h, fmt = img.info(cand)
  if w then
    g, kind = img.load(cand), fmt
    name = cand
    words = string.format("%s  %dx%d  %s", cand, w, h, fmt)
    break
  end
end

if not g then
  -- no file: a 64x48 test pattern built with the ordinary GROB API, so the
  -- blit calls below still run
  g = gfx.newgrob(64, 48)
  gfx.select(g)
  for y = 0, 47 do
    for x = 0, 63 do
      local c = gfx.rgb(x * 4 % 256, y * 5 % 256, (x + y) * 3 % 256)
      gfx.pixel(x, y, c)
    end
  end
  gfx.fillcircle(20, 16, 12, gfx.rgb(255, 0, 0))
  gfx.fillrect(30, 20, 30, 24, gfx.rgb(0, 128, 255))
  gfx.select(false)
  name, words, kind = "(built in memory)", "no image file found, built one", "grob"
end

local gw, gh = gfx.grobwidth(g), gfx.grobheight(g)
mark("loaded " .. tostring(name) .. " " .. gw .. "x" .. gh)
print(string.format("imgdemo: %s %dx%d (%s), interp %s", tostring(name), gw, gh,
                    tostring(kind), sys.version and sys.version() or "?"))

local half = img.scale(g, math.max(1, gw // 2), math.max(1, gh // 2))
local big = img.scale(g, gw * 2, gh * 2)

-- ---------------------------------------------------------------------------
-- draw once, then wait for ESC
-- ---------------------------------------------------------------------------
local function frame()
  gfx.clear(gfx.rgb(24, 24, 32))
  gfx.text(4, 4, "img.load / blit / strblit", gfx.WHITE)
  gfx.text(4, 14, words, gfx.GRAY)

  -- 1:1
  img.blit(g, 8, 40)
  gfx.rect(7, 39, gw + 2, gh + 2, gfx.GRAY)
  gfx.text(8, 40 + gh + 2, "blit", gfx.WHITE)

  -- scaled up (2x) and down (1/2), both from the same source
  img.strblit(g, 100, 40, gw * 2, gh * 2)
  gfx.text(100, 40 + gh * 2 + 2, "strblit 2x", gfx.WHITE)

  img.blit(big, 8, 140)
  gfx.text(8, 140 + gh * 2 + 2, "scale(2x) once, blit", gfx.WHITE)

  -- a sub-rectangle, scaled: 16x16 from the middle onto 48x48
  img.strblitpart(g, 250, 40, 48, 48, gw / 2 - 8, gh / 2 - 8, 16, 16)
  gfx.text(250, 92, "strblitpart", gfx.WHITE)

  -- 1/2 via scale(), and a colour-keyed blit (black is transparent here)
  img.blit(half, 100, 140)
  img.blit(g, 160, 140, gfx.rgb(0, 0, 0))
  gfx.text(100, 140 + gh // 2 + 2, "scale(0.5)  |  blit with key", gfx.WHITE)

  gfx.text(4, H - 12, string.format("screen %dx%d   ESC = exit", W, H), gfx.GRAY)
end

mark("first frame")
frame()
mark("first frame drawn")

key.install()
local ticks, quit = 0, false
while not quit and ticks < MAX_TICKS do
  ticks = ticks + 1
  if key.getkey(TICK_MS) == key.ESC then
    quit = true
  end
end
key.remove()

gfx.clear(gfx.rgb(24, 24, 32))
gfx.text(4, 4, quit and "imgdemo: bye" or "imgdemo: time limit", gfx.GREEN)
mark(string.format("done: %d tick(s), %s", ticks, quit and "ESC" or "limit"))
print(string.format("imgdemo: %d tick(s), %s", ticks,
                    quit and "ended on ESC" or "hit the time limit"))
