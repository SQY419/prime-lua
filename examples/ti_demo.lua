--# no-compare
--# expect-exit: 0
--# max-insn: 900000000
-- ti_demo.lua -- a TI-Nspire style script, running on the Prime through lib/ti.lua.
--
-- This is written the way a TI-Nspire Lua program is written: the handlers hang
-- off the `on` table, drawing happens in on.paint(gc), animation is driven by
-- on.timer, and the state lives in `var` so it survives a run.  Only TWO lines
-- are primeLua-specific:
--
--     local ti = local ti = require("ti")   -- publishes on/platform/gc/color/timer/var/keys/...
--     ti.run()                   -- the Nspire OS would call the handlers; here
--                                   ti.run() is the event loop (ESC leaves it)
--
-- Everything else is plain TI Lua API: platform.window:width()/invalidate(),
-- gc drawing calls, color.*, timer.start, var.store/recall, keys.*.
--
--   arrows      push the ball around
--   ENTER       reset it to the middle
--   any letter  stamp a copy of the logo image where the ball is
--   ESC         leave (always available, see the safety note in lib/ti.lua)
--
-- The ball bounces off the walls, the trail is drawn as a dashed line, and the
-- bottom row shows the primitives a TI script expects to exist: a polygon, an
-- arc, a clipped rectangle and an image built pixel by pixel with image.new.

require("ti")

local W, H
local ball = { x = 0, y = 0, dx = 0.9, dy = 0.7, r = 7 }
local stamps = {}
local logo
local hue = 0

-- a small bitmap built with image.new/setPixel, the way a TI script would use a
-- TI.Image: a 5x5 cyan cross
local function make_logo()
  local img = image.new(5, 5)
  for i = 0, 4 do
    image.setPixel(img, i, 2, color.cyan)
    image.setPixel(img, 2, i, color.cyan)
  end
  return img
end

function on.construction()
  W, H = platform.window:width(), platform.window:height()
  logo = make_logo()
  -- var keeps the ball where it was when the program last ran
  local saved = var.recall("ti_demo_ball")
  if type(saved) == "table" then
    ball.x, ball.y = saved[1] or W / 2, saved[2] or H / 2
    ball.dx, ball.dy = saved[3] or 0.9, saved[4] or 0.7
  else
    ball.x, ball.y = W / 2, H / 2
  end
  timer.start(1 / 30)                     -- 30 frames a second, in seconds
end

function on.resize(w, h)
  W, H = w, h
end

function on.arrowKey(key)
  local push = 0.8
  if key == "left" then ball.dx = ball.dx - push
  elseif key == "right" then ball.dx = ball.dx + push
  elseif key == "up" then ball.dy = ball.dy - push
  elseif key == "down" then ball.dy = ball.dy + push
  end
end

function on.enterKey()
  ball.x, ball.y = W / 2, H / 2
  ball.dx, ball.dy = 0.9, 0.7
  stamps = {}
end

function on.charIn()
  stamps[#stamps + 1] = { x = ball.x, y = ball.y }
  if #stamps > 12 then table.remove(stamps, 1) end
end

function on.timer()
  ball.x = ball.x + ball.dx
  ball.y = ball.y + ball.dy
  if ball.x < ball.r then ball.x, ball.dx = ball.r, -ball.dx end
  if ball.x > W - ball.r then ball.x, ball.dx = W - ball.r, -ball.dx end
  if ball.y < ball.r + 24 then ball.y, ball.dy = ball.r + 24, -ball.dy end
  if ball.y > H - ball.r - 18 then
    ball.y, ball.dy = H - ball.r - 18, -ball.dy
  end
  var.store("ti_demo_ball", { ball.x, ball.y, ball.dx, ball.dy })
  platform.window:invalidate()            -- this is what fires on.paint
end

function on.paint(gc)
  hue = (hue + 7) % 256
  gc:setColorRGB(16, 20, 32)
  gc:fillRect(0, 0, W, H)                 -- the window is cleared by ti.lua

  -- a dashed ruler along the top, a polygon and an arc along the bottom: the
  -- primitives a TI script uses without a second thought
  gc:setColorRGB(80, 80, 96)
  gc:setPen("thin", "dashed")
  gc:drawLine(0, 22, W, 22)
  gc:setPen("thin", "smooth")

  gc:setColorRGB(40, 120, 200)
  gc:fillPolygon({ 6, H - 16, 40, H - 16, 23, H - 6 })
  gc:setColorRGB(200, 120, 40)
  gc:drawArc(W - 44, H - 22, 36, 16, 0, 180)

  gc:setColorRGB(70, 70, 90)
  gc:clipRect(1, 24, W - 2, H - 44)       -- the playfield
  gc:drawRect(1, 24, W - 3, H - 45)
  gc:clipRect()

  -- the stamped logos, then the ball itself
  for _, s in ipairs(stamps) do
    gc:drawImage(logo, s.x - 2, s.y - 2)
  end
  gc:setColorRGB(hue, 200 - hue // 2, 255 - hue)
  gc:fillCircle(ball.x, ball.y, ball.r)

  -- a trail, drawn with the pen the ball is moving along
  gc:setPen("medium", "dotted")
  gc:drawLine(ball.x, ball.y, ball.x - ball.dx * 14, ball.y - ball.dy * 14)
  gc:setPen("thin", "smooth")

  -- text via the gc, anchored the way the Nspire anchors it
  gc:setColorRGB(color.white)
  gc:setFont("sansserif", "r", 10)
  gc:drawString("TI-Nspire API on the Prime", 4, 14, "bottom")
  gc:setColorRGB(color.gray)
  gc:drawString("arrows push, ENTER resets, a letter stamps, ESC leaves",
                4, H - 3, "bottom")
end

-- the only primeLua-specific call besides require("ti"): the Nspire OS owns the
-- event loop, here the script runs it.  120 s is a safety bound; ESC is always
-- available because ti.lua quits on it unless the script asks it not to.
ti.run({ seconds = 120 })
