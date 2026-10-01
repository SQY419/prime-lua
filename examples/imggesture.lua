--# no-compare
--# expect-exit: 0
--# max-insn: 600000000
-- imggesture.lua -- pinch-zoom and pan an image with two fingers (tests `img`).
--
--   two fingers, moving apart / together   zoom (about the midpoint)
--   two fingers, both moving the same way  pan
--   one finger                             pan
--   ENTER                                  fit to the screen
--   +/-                                    zoom in / out by 25%
--   ESC                                    leave
--
-- THE GESTURE READS key.contacts(), NOT the touch events.  key.event() reports
-- the fingers OF ONE FRAME, which is not the same thing: a frame that carries
-- only finger 2's move says nothing about finger 1, so a pinch built on
-- consecutive frames keeps forgetting the finger that held still.  key.contacts()
-- is the LIVE table -- press adds, move updates, release removes -- which is
-- what a gesture actually wants, and it costs one table per iteration instead of
-- one per event.
--
-- That table is fed by primeLua's own view of the firmware's touch frames
-- (port/plua_touch.c, filled by the hook in primetcc's rt/hp_input_svc.c).  The
-- hook used to rewrite every frame to count = 1 and drop moves, which is why a
-- two-finger pinch could never be seen from Lua; the top-left line now shows the
-- live contact count and the most the panel has reported this run, so "the
-- hardware does not report two fingers" and "we dropped the second finger" can
-- be told apart without a cable.
--
-- Keys still come from key.event(): keys and touch frames share one queue, and
-- the single-kind pollers throw away what they do not want, so a demo that also
-- listens for ESC has to read the one poller that returns everything.
--
-- NO FLICKER: the screen is repainted only when something actually changed, at
-- most every PAINT_MS, and without ever clearing the whole screen -- a
-- clear-then-draw loop at 15 ms a tick flickers badly on the LCD.  Only the
-- strips around the image are cleared.  (A 320x240 back buffer plus a
-- full-screen blit would remove the flicker entirely, but that pair is what was
-- REBOOTING the calculator on real hardware, so this demo does not use it.)
--
-- THIS IS A REAL MULTI-TOUCH GESTURE, not a simulation: the Prime's panel
-- reports up to EIGHT simultaneous touch items per event frame, each with its
-- own press/move/release type, and key.gettouches() hands Lua the whole frame.
-- (key.gettouch() only ever returned the first one.)  The screen shows the
-- current touch points as small circles, so it is obvious which fingers the
-- firmware is reporting.
--
-- Every frame draws the image with img.strblitpart(), because a zoomed image is
-- mostly off-screen and the library should not be asked to walk millions of
-- destination pixels: lib/zoom.lua computes the destination rectangle clipped
-- to the viewport plus the matching SOURCE rectangle, so the cost per frame is
-- at most 320x240 pixels no matter how far in the image is zoomed.  That math
-- is tested on its own in tests/zoom_host.lua (make test-zoom).
--
-- The image: "imgdemo.jpg" / "imgdemo.png" / "bg.jpg" / "photo.jpg" ... in the
-- app folder, else a test pattern built in memory -- so the demo always has
-- something to pinch.  ESC leaves; the loop is bounded.

local gfx, key, sys, img = gfx, key, sys, img
local zoom = require("zoom")            -- lib/zoom.lua (deployed next to this)

local CANDIDATES = { "imgdemo.jpg", "imgdemo.png", "bg.jpg", "bg.png",
                     "photo.jpg", "photo.png", "test.png" }
local MAX_TICKS = 1200
local TICK_MS = 15
local MIN_S, MAX_S = 0.2, 24

-- `imggesture.lua -d` (or -v/--debug): every milestone goes to the DURABLE log
-- plua.log, survives a reboot, and is echoed to the console.  The hardware
-- libraries are documented as "log before anything risky", and this demo is
-- the one that has to prove touch works -- a silent failure here is the bug
-- report that costs an hour.  Arguments arrive as the global `arg`.
local DEBUG = false
for _, a in ipairs(arg or {}) do
  if a == "-d" or a == "-v" or a == "--debug" then DEBUG = true end
end
local marks = {}

local function mark(s)
  marks[#marks + 1] = s
  if DEBUG then
    if sys.log then sys.log("imggesture: " .. s) end
    print("imggesture: " .. s)
  end
end

if not gfx.init() then
  print("imggesture: no display")
  return
end
gfx.select(false)
local W, H = gfx.width(), gfx.height()

-- ---------------------------------------------------------------------------
-- the image
-- ---------------------------------------------------------------------------
local pic, name = nil, nil
for _, cand in ipairs(CANDIDATES) do
  local w, h = img.info(cand)
  if w then
    pic, name = img.load(cand), cand
    break
  end
end
if not pic then
  pic = gfx.newgrob(120, 90)
  gfx.select(pic)
  for y = 0, 89 do
    for x = 0, 119 do
      gfx.pixel(x, y, gfx.rgb(x * 2 % 256, y * 3 % 256, (x + y) % 256))
    end
  end
  gfx.fillcircle(30, 25, 18, gfx.rgb(255, 40, 40))
  gfx.fillrect(60, 40, 50, 40, gfx.rgb(40, 120, 255))
  gfx.select(false)
  name = "(built in memory)"
end
local IW, IH = gfx.grobwidth(pic), gfx.grobheight(pic)
mark(string.format("image %s %dx%d", tostring(name), IW, IH))

-- ---------------------------------------------------------------------------
-- the gesture state
-- ---------------------------------------------------------------------------
local view = zoom.clamp(zoom.fit(IW, IH, W, H, 6), IW, IH, W, H)
local prev = nil            -- { {x=,y=}, ... } the live contacts last iteration
local fingers = {}          -- the live contacts, for drawing
local frames = 0
local max_fingers = 0       -- the most contacts the panel has reported this run
local mode = "pan"

local TOUCH_MATCH = 80      -- px: which previous contact a current one is

local function set_view(v, why)
  local before = string.format("%.2f", view.s)
  view = zoom.clamp(v, IW, IH, W, H)
  if string.format("%.2f", view.s) ~= before or why == "fit" then
    mark(string.format("%s: scale %s -> %.2f", why, before, view.s))
  end
end

-- Apply the LIVE contact table.  Contacts keep their slots while a finger stays
-- down, but a finger can be released from the middle of the table, so a current
-- contact is matched to the previous one by position (nearest wins) instead of
-- by index: getting that wrong makes the view jump when the second finger lifts.
local function match_prev(pts)
  local out = {}
  if not prev then return out end
  for i = 1, #pts do
    local best, bestd = nil, TOUCH_MATCH * TOUCH_MATCH
    for j = 1, #prev do
      local dx, dy = pts[i].x - prev[j].x, pts[i].y - prev[j].y
      local d = dx * dx + dy * dy
      if d < bestd then best, bestd = j, d end
    end
    out[i] = best and prev[best] or nil
  end
  return out
end

-- returns true when the view changed
local function on_contacts(pts)
  fingers = pts
  if #pts > max_fingers then
    max_fingers = #pts
    mark("contacts seen: " .. max_fingers)
  end
  if #pts == 0 then
    local had = prev ~= nil
    prev = nil
    mode = "pan"
    return had                        -- repaint, so the markers go away
  end

  local old = match_prev(pts)
  if #pts >= 2 then
    local i, j
    for a = 1, #pts do                 -- the two OLDEST contacts, matched
      if old[a] then
        if not i then i = a elseif not j then j = a end
      end
    end
    if i and j then
      -- two fingers: pinch, which is also a pan when the distance holds
      set_view(zoom.pinch(view, old[i].x, old[i].y, old[j].x, old[j].y,
                          pts[i].x, pts[i].y, pts[j].x, pts[j].y,
                          MIN_S, MAX_S), "pinch")
      mode = "pinch"
      prev = pts
      return true
    end
    mode = "pinch"
    prev = pts
    return false
  end

  if old[1] then
    local dx, dy = pts[1].x - old[1].x, pts[1].y - old[1].y
    prev = pts
    if dx ~= 0 or dy ~= 0 then
      set_view(zoom.pan(view, dx, dy), "pan")
      mode = "pan"
      return true
    end
    return false
  end
  prev = pts
  mode = "pan"
  return false
end

-- ---------------------------------------------------------------------------
-- drawing
-- ---------------------------------------------------------------------------
local BG = gfx.rgb(18, 18, 24)

local function frame()
  local dx, dy, dw, dh, sx, sy, sw, sh = zoom.visible(view, IW, IH, W, H)
  if dx then
    img.strblitpart(pic, dx, dy, dw, dh, sx, sy, sw, sh)
    -- clear only the strips AROUND the image: a full gfx.clear() every frame is
    -- what made this flicker (the LCD shows the cleared screen for a moment)
    if dy > 0 then gfx.fillrect(0, 0, W, dy, BG) end
    if dy + dh < H then gfx.fillrect(0, dy + dh, W, H - (dy + dh), BG) end
    if dx > 0 then gfx.fillrect(0, dy, dx, dh, BG) end
    if dx + dw < W then gfx.fillrect(dx + dw, dy, W - (dx + dw), dh, BG) end
    gfx.rect(dx, dy, dw - 1, dh - 1, gfx.rgb(70, 70, 90))
  else
    gfx.fillrect(0, 0, W, H, BG)
  end

  -- the live contacts the panel is reporting
  for _, p in ipairs(fingers) do
    gfx.fillcircle(p.x, p.y, 6, gfx.YELLOW)
    gfx.circle(p.x, p.y, 8, gfx.WHITE)
  end

  gfx.fillrect(0, 0, W, 12, gfx.rgb(0, 0, 0))
  gfx.text(3, 3, string.format("%s %dx%d  zoom %.2fx  %s  fingers %d/%d",
                               name, IW, IH, view.s, mode, #fingers,
                               max_fingers), gfx.WHITE)
  gfx.fillrect(0, H - 11, W, 11, gfx.rgb(0, 0, 0))
  gfx.text(3, H - 10,
           "1 finger=pan  2 fingers=zoom/pan  ENTER=fit  ESC", gfx.GRAY)
end

mark("first frame")
frame()
mark("first frame drawn")

if not key.install() then
  -- no hook means no input at all: report it and leave instead of sleeping
  -- through a screenful of nothing
  local msg = "imggesture: the input hook could NOT be installed -- the "
    .. "firmware's get_event slot is patched by an older session. Reboot the "
    .. "calculator, or run 'u' in the launcher to unload the image, then run "
    .. "this again."
  if sys.log then sys.log("imggesture: " .. msg) end
  gfx.clear(gfx.rgb(18, 18, 24))
  gfx.text(4, 4, "no input hook: reboot", gfx.RED)
  print(msg)
  return
end
local ticks, quit = 0, false
local K_ESC, K_ENTER = key.ESC, key.ENTER
local PAINT_MS = 40                   -- ~25 fps while dragging
local last_paint = os.clock() - 1

local function zoom_by(factor, why)
  set_view(zoom.pinch(view, 0, 0, 100, 0, 0, 0, 100 * factor, 0, MIN_S, MAX_S),
           why)
end

while not quit and ticks < MAX_TICKS do
  ticks = ticks + 1
  local dirty = false

  local ev = key.event()
  while ev do
    if ev.type == "key" and ev.down then
      local kc = ev.code
      if kc == K_ESC then
        quit = true
      elseif kc == K_ENTER then
        set_view(zoom.fit(IW, IH, W, H, 6), "fit")
        dirty = true
      elseif kc == 0x2B or kc == 0x3D then        -- '+' or '='
        zoom_by(1.25, "zoom+")
        dirty = true
      elseif kc == 0x2D then                      -- '-'
        zoom_by(0.8, "zoom-")
        dirty = true
      end
    end
    ev = key.event()
  end

  -- the events have been drained, so the contact table is up to date: read it
  -- ONCE per iteration (it is the whole picture, not one frame's worth)
  local pts = key.contacts()
  if #pts ~= #fingers or #pts > 0 then
    dirty = on_contacts(pts) or dirty
  end

  -- repaint only when something changed, and at most every PAINT_MS: painting
  -- on a fixed cadence is what flickers, painting on every touch frame is what
  -- makes a drag run at the panel's event rate instead of the screen's
  local now = os.clock()
  if dirty and (now - last_paint) >= PAINT_MS / 1000 then
    frames = frames + 1
    last_paint = now
    frame()
  end
  sys.sleep(TICK_MS)
end

key.remove()
if DEBUG then
  -- the two numbers that say whether the PANEL or primeLua is at fault: if
  -- max_contacts stays 1 after a deliberate two-finger pinch, the firmware
  -- never reported a second contact.
  local hs = key.touchstats()
  mark(string.format("hook: %s, touch frames %d, items/frame <= %d, "
                     .. "contacts <= %d, expired %d",
                     tostring(key.hooked()), hs.frames, hs.max_items,
                     hs.max_contacts, hs.expired))
  mark(string.format("view: zoom %.3f offset %.1f,%.1f, %d frame(s)",
                     view.s, view.x, view.y, frames))
  if #marks > 0 then
    print("imggesture: --- marks ---")
    for _, m in ipairs(marks) do print("  " .. m) end
  end
end
gfx.clear(gfx.rgb(18, 18, 24))
gfx.text(4, 4, string.format("imggesture: %d frame(s), zoom %.2fx",
                             frames, view.s), gfx.GREEN)
mark(string.format("done: %d frame(s), %s, zoom %.2f", frames,
                   quit and "ESC" or "limit", view.s))
mark(string.format("contacts: live %d, max %d", #fingers, max_fingers))
local st = key.touchstats()
print(string.format("imggesture: %s %dx%d, %d frame(s), final zoom %.3f "
                    .. "offset %.1f,%.1f",
                    tostring(name), IW, IH, frames, view.s, view.x, view.y))
print(string.format("imggesture: touch frames %d, max items %d, max contacts "
                    .. "%d, contacts seen %d, expiry %d, %s",
                    st.frames, st.max_items, st.max_contacts, max_fingers,
                    st.expired, quit and "ended on ESC" or "hit the time limit"))
