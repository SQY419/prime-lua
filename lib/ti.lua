--[[----------------------------------------------------------------------------
  ti.lua -- a TI-Nspire ("TI Lua") compatibility layer for primeLua.

  Goal: take a script written for the TI-Nspire Lua API -- the one documented on
  https://wiki.inspired-lua.org/ -- and run it on the HP Prime, so
      require("ti")
      function on.paint(gc) ... end
  is all a TI script needs on top of its own code.

  WHAT IS PROVIDED (and how it maps onto primeLua)
    platform            window: width/height/invalidate/setBackgroundColor/
                        setFocus, isColorDisplay/isDeviceModeRendering/
                        isTabletModeRendering, hw, apiLevel, withGC,
                        registerErrorHandler, getString
    on                  the event table: construction (or the older name
                        on.create, which is accepted as an alias), resize,
                        activate, deactivate, destroy, paint, timer, charIn,
                        arrowKey/arrowUp/Down/Left/Right, enterKey, returnKey,
                        escapeKey, tabKey, backspaceKey, contextMenu, help,
                        mouseDown/Up/Move, varChange, save/restore, copy/cut/
                        paste, getSymbolList
    gc                  setColorRGB/setColor/setPen/setFont/setAlpha, drawString,
                        drawLine/drawRect/fillRect/drawCircle/fillCircle,
                        drawPolyLine/drawPolygon/fillPolygon, drawArc/fillArc,
                        drawImage, getStringWidth/getStringHeight, clipRect
    color               the named colours (black, white, red, green, blue,
                        yellow, cyan, magenta, gray, orange, purple, brown,
                        pink, lightgray, darkgray)
    timer               start(seconds)/stop/getMilliSecCounter/
                        getMillisecondsSinceEpoch
    var                 store/recall/recallstr/list/recallAt/storeAt/
                        makeNumericList/monitor/unmonitor/ignore/unignore --
                        numbers, strings and tables, PERSISTED to a file so a
                        value survives the run (TI persists it in the document)
    keys                the TI key names, holding primeLua's device key codes
    image               new/copy/width/height/rotate/getPixel/setPixel + the
                        colon form (img:width()); draw with gc:drawImage
    locale, clipboard   locale.name(); clipboard.addText/getText are accepted
                        but do nothing (the Prime exposes no clipboard to Lua)
    string/math         string.split, string.uchar, string.usub; math.eval /
                        math.evalStr evaluate expressions with Lua itself, and
                        math.getEvalSettings/setEvalSettings are recorded but
                        not used (there is no CAS bridge -- see below)

  WHAT IS DELIBERATELY NOT PROVIDED (it is TI-document machinery)
    document, cursor, d2Editor, toolpalette, physics (Chipmunk).  A script that
    needs them fails with "attempt to index a nil value" naming the library,
    which is the honest outcome; everything a *graphics* script needs is here.

  KNOWN DIFFERENCES FROM THE NSPIRE (all deliberate)
    * Fonts: the image has one size per face (16 px Montserrat / JetBrains Mono /
      Cascadia, plus a 56 px digit face).  gc:setFont's family picks the face and
      its SIZE is ignored -- ask gc:getStringHeight() for the truth.  No bold or
      italic faces.
    * Coordinates: the Nspire screen is 320x240 and so is the Prime's, so
      platform.window:width()/height() return the real window and scripts that
      centre themselves work unchanged.
    * gc:setAlpha is accepted and ignored (TI deprecated it in 3.2).
    * Pen thickness ("thin"/"medium"/"thick") and "dashed"/"dotted" are EMULATED:
      a thick line is several offset lines, a dashed one is cut into segments.
    * Arcs are drawn as polylines (0 deg = 3 o'clock, counter-clockwise), and
      fillArc approximates the pie with radial lines.
    * clipRect stores one rectangle and applies it to rectangles and strings;
      lines and circles are only culled by their bounding box.
    * math.eval uses Lua's own evaluator, so numeric/string expressions work but
      CAS-only input (solve, symbolic integration, ...) does not: it returns nil.
      That is the one TI feature that cannot be faked, because the Prime's CAS is
      not reachable from a Lua script.
    * timer.getMilliSecCounter() counts from when the interpreter started (the
      firmware exposes no "since boot" counter to Lua).

  EVENT LOOP
    The Nspire OS drives the script; on the Prime the script drives itself, so
    ti.run() is the event loop: it INSTALLS THE INPUT HOOK (key.install(), and
    key.remove() on the way out), polls it, runs the timer and calls
    on.paint(gc) whenever something asked for a repaint (platform.window:
    invalidate()).  The loop is BOUNDED (ti.run{seconds = n}, 5 minutes by
    default) and ESC always leaves it, unless the script passes
    ti.run{esc_stays = true}: the launcher's Python side is blocked inside the
    run, so a script must never be able to trap the calculator.

    A TI script therefore ends with its handlers plus three lines:

        require("ti")
        function on.paint(gc) ... end
        ti.run()                     -- or: ti.runfile("myscript.lua") driver

    ti.step(ms) is one iteration of that loop, which is what the host test drives.
    ti.stop() asks the loop to return; ti.quitting is the matching state.

    If a converted script "runs but nothing responds", the run's own numbers say
    which half is broken: ti.run{diag = true} prints (and logs to plua.log) how
    many keys and touches arrived, how many handler errors there were, and what
    the panel reported (key.touchstats().max_contacts).  Zero events means the
    input hook, not the script.
----------------------------------------------------------------------------]]

local M = {}

M.VERSION = "0.1"
M.API_LEVEL = "2.4"          -- what platform.apiLevel reports

-- hardware handles: primeLua registers these as globals; naming them locally
-- keeps the lookup out of the hot drawing path
local gfx, key, sys, font = gfx, key, sys, font

local COLOR_BLACK = 0x000000

-- ---------------------------------------------------------------------------
-- small helpers
-- ---------------------------------------------------------------------------
local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function log_error(where, err)
  local msg = string.format("ti: error in %s: %s", where, tostring(err))
  if sys and sys.log then
    sys.log(msg)
  end
  print(msg)
end

-- ---------------------------------------------------------------------------
-- color: the named colours a TI script uses
-- ---------------------------------------------------------------------------
local color = {
  black = 0x000000, white = 0xFFFFFF,
  red = 0xFF0000, green = 0x00FF00, blue = 0x0000FF,
  yellow = 0xFFFF00, cyan = 0x00FFFF, magenta = 0xFF00FF,
  gray = 0x808080, orange = 0xFF8000, purple = 0x800080,
  brown = 0x800000, pink = 0xFFAFAF, lightgray = 0xC0C0C0,
  darkgray = 0x404040,
}
M.color = color

-- ---------------------------------------------------------------------------
-- gc: the graphics context (one instance, handed to every on.paint)
-- ---------------------------------------------------------------------------
local gc = {}
gc.__index = gc

local pen = { thick = 1, style = "smooth" }   -- thickness in px, dash style
local font_handle = nil                       -- current primeLua font
local text_face = "sansserif"
local clip = nil                              -- {x=,y=,w=,h=} or nil
local cur_color = COLOR_BLACK
local cur_alpha = 255

local function ensure_font()
  if font_handle then return font_handle end
  if text_face == "monospace" then
    font_handle = (font.cascadia and font.cascadia()) or font.mono()
  else
    font_handle = font.prop()
  end
  return font_handle
end

-- the line height is all the public API gives; the baseline sits at ~0.76 of it
-- on every face in the image (prop 16/21, mono 15/19, cascadia 15/19, big 52/66)
local function face_ascent(h)
  if not h then return 0 end
  return math.floor(h * 0.76 + 0.5)
end

local function in_clip(x, y)
  if not clip then return true end
  return x >= clip.x and y >= clip.y
    and x < clip.x + clip.w and y < clip.y + clip.h
end

local function clipped_rect(x, y, w, h)
  if not clip then return x, y, w, h, true end
  local x1 = math.max(x, clip.x)
  local y1 = math.max(y, clip.y)
  local x2 = math.min(x + w, clip.x + clip.w)
  local y2 = math.min(y + h, clip.y + clip.h)
  if x2 <= x1 or y2 <= y1 then return 0, 0, 0, 0, false end
  return x1, y1, x2 - x1, y2 - y1, true
end

function gc:setColorRGB(r, g, b)
  cur_color = (clamp(r or 0, 0, 255) << 16)
    | (clamp(g or 0, 0, 255) << 8) | clamp(b or 0, 0, 255)
end

function gc:setColor(c)
  cur_color = c or COLOR_BLACK
end

function gc:setAlpha(a)
  cur_alpha = clamp(a or 255, 0, 255)     -- accepted, not used: see the header
end

function gc:setPen(thickness, smoothness)
  if thickness == "medium" then pen.thick = 3
  elseif thickness == "thick" then pen.thick = 5
  else pen.thick = 1 end
  pen.style = smoothness or "smooth"
end

function gc:setFont(family, style, size)
  text_face = (family == "monospace") and "monospace" or "sansserif"
  font_handle = nil                       -- rebuilt on the next drawString
  return family, style, size
end

function gc:clipRect(op, x, y, w, h)
  if x == nil or w == nil or w <= 0 or h == nil or h <= 0 then
    clip = nil
    return
  end
  clip = { x = x, y = y, w = w, h = h }   -- op is accepted and ignored
end

function gc:getStringWidth(s)
  return font.width(ensure_font(), s)
end

function gc:getStringHeight(s)
  return font.height(ensure_font())
end

-- draw a string at (x, y) with the Nspire's anchor names
function gc:drawString(s, x, y, position)
  s = tostring(s or "")
  local h = font.height(ensure_font())
  local top = y
  if position == "baseline" then
    top = y - face_ascent(h)
  elseif position == "bottom" then
    top = y - h
  elseif position == "middle" then
    top = y - math.floor(h / 2)
  end
  if clip then
    if not in_clip(x, top) and not in_clip(x, top + h - 1) then
      if (top + h) < clip.y or top > (clip.y + clip.h)
        or x > (clip.x + clip.w) or (x + self:getStringWidth(s)) < clip.x then
        return
      end
    end
  end
  font.draw(ensure_font(), x, top, s, cur_color)
end

-- lines: thickness and dash style are emulated (see the header)
local function raw_line(x1, y1, x2, y2)
  gfx.line(x1, y1, x2, y2, cur_color)
end

local function dashed_line(x1, y1, x2, y2)
  local dx, dy = x2 - x1, y2 - y1
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 1 then
    raw_line(x1, y1, x2, y2)
    return
  end
  local on, off = (pen.style == "dotted") and 1 or 4,
                  (pen.style == "dotted") and 3 or 4
  local step = on + off
  local t = 0
  while t < len do
    local t2 = math.min(t + on, len)
    raw_line(x1 + dx * t / len, y1 + dy * t / len,
             x1 + dx * t2 / len, y1 + dy * t2 / len)
    t = t + step
  end
end

local function thick_line(x1, y1, x2, y2)
  local k = pen.thick
  if k <= 1 then
    raw_line(x1, y1, x2, y2)
    return
  end
  local r = (k - 1) / 2
  for dy = -r, r do
    for dx = -r, r do
      if math.abs(dx) + math.abs(dy) <= r then
        raw_line(x1 + dx, y1 + dy, x2 + dx, y2 + dy)
      end
    end
  end
end

function gc:drawLine(x1, y1, x2, y2)
  if pen.style == "dashed" or pen.style == "dotted" then
    dashed_line(x1, y1, x2, y2)
  else
    thick_line(x1, y1, x2, y2)
  end
end

function gc:drawRect(x, y, w, h)
  local k = pen.thick
  for i = 0, k - 1 do
    local cx, cy, cw, ch, ok = clipped_rect(x + i, y + i, w - 2 * i, h - 2 * i)
    if ok and cw > 0 and ch > 0 then
      gfx.rect(cx, cy, cw, ch, cur_color)
    end
  end
end

function gc:fillRect(x, y, w, h)
  local cx, cy, cw, ch, ok = clipped_rect(x, y, w, h)
  if ok and cw > 0 and ch > 0 then
    gfx.fillrect(cx, cy, cw, ch, cur_color)
  end
end

function gc:drawCircle(x, y, r)
  local k = pen.thick
  if k <= 1 then
    gfx.circle(x, y, r, cur_color)
    return
  end
  for i = 0, k - 1 do
    gfx.circle(x, y, r + i - (k - 1) / 2, cur_color)
  end
end

function gc:fillCircle(x, y, r)
  gfx.fillcircle(x, y, r, cur_color)
end

local function poly_points(list)
  -- the Nspire accepts either one flat list {x1,y1,x2,y2,...} or several
  local pts = {}
  if type(list[1]) == "table" then
    for _, sub in ipairs(list) do
      for i = 1, #sub do pts[#pts + 1] = sub[i] end
    end
  else
    for i = 1, #list do pts[i] = list[i] end
  end
  -- only keep complete (x, y) pairs: a trailing or missing coordinate would
  -- otherwise reach the scanline loop as a nil
  local out = {}
  for i = 1, #pts - 1, 2 do
    if type(pts[i]) == "number" and type(pts[i + 1]) == "number" then
      out[#out + 1] = pts[i]
      out[#out + 1] = pts[i + 1]
    end
  end
  return out
end

function gc:drawPolyLine(...)
  local pts = poly_points({ ... })
  if #pts < 4 then return end
  for i = 1, #pts - 3, 2 do
    self:drawLine(pts[i], pts[i + 1], pts[i + 2], pts[i + 3])
  end
end

function gc:drawPolygon(...)
  local pts = poly_points({ ... })
  if #pts < 6 then return end
  local n = #pts
  for i = 1, n - 3, 2 do
    self:drawLine(pts[i], pts[i + 1], pts[i + 2], pts[i + 3])
  end
  if n >= 4 then
    self:drawLine(pts[n - 1], pts[n], pts[1], pts[2])
  end
end

-- even-odd scanline fill: a polygon fill without a rasterizer in the firmware
function gc:fillPolygon(...)
  local pts = poly_points({ ... })
  local n = #pts
  if n < 6 then return end
  local ymin, ymax = pts[2], pts[2]
  for i = 2, n, 2 do
    ymin = math.min(ymin, pts[i])
    ymax = math.max(ymax, pts[i])
  end
  ymin, ymax = math.floor(ymin), math.ceil(ymax)
  for y = ymin, ymax do
    local xs = {}
    for i = 1, n, 2 do
      local k = (i + 2 > n) and 1 or (i + 2)        -- wrap to the first vertex
      local yi, yj = pts[i + 1], pts[k + 1]
      local xi, xj = pts[i], pts[k]
      if (yi <= y and yj > y) or (yj <= y and yi > y) then
        xs[#xs + 1] = xi + (y - yi) * (xj - xi) / (yj - yi)
      end
    end
    table.sort(xs)
    for k = 1, #xs - 1, 2 do
      local x1 = math.floor(xs[k] + 0.5)
      local x2 = math.floor(xs[k + 1] + 0.5)
      if x2 > x1 then
        self:fillRect(x1, y, x2 - x1, 1)
      end
    end
  end
end

-- arcs: 0 degrees is 3 o'clock, angles grow counter-clockwise
local function arc_point(cx, cy, rx, ry, deg)
  local r = math.rad(deg)
  return cx + rx * math.cos(r), cy - ry * math.sin(r)
end

function gc:drawArc(x, y, w, h, a0, a1)
  local rx, ry = w / 2, h / 2
  local cx, cy = x + rx, y + ry
  local pts = {}
  local steps = math.max(8, math.floor(math.abs(a1 - a0) / 5))
  for i = 0, steps do
    local px, py = arc_point(cx, cy, rx, ry, a0 + (a1 - a0) * i / steps)
    pts[#pts + 1] = px
    pts[#pts + 1] = py
  end
  self:drawPolyLine(pts)
end

function gc:fillArc(x, y, w, h, a0, a1)
  local rx, ry = w / 2, h / 2
  local cx, cy = x + rx, y + ry
  local steps = math.max(8, math.floor(math.abs(a1 - a0) / 3))
  for i = 0, steps do
    local px, py = arc_point(cx, cy, rx, ry, a0 + (a1 - a0) * i / steps)
    self:drawLine(cx, cy, px, py)
  end
end

function gc:drawImage(img, x, y)
  if type(img) ~= "table" or not img.data then
    error("gc:drawImage expects an image from image.new()", 2)
  end
  gfx.blitpixels(img.data, img.w, img.h, x, y)
end

M.gc = setmetatable(gc, gc)

-- ---------------------------------------------------------------------------
-- image
-- ---------------------------------------------------------------------------
local image = {}
image.__index = image

local function image_blank(w, h, c)
  w, h = math.max(1, math.floor(w or 1)), math.max(1, math.floor(h or 1))
  local px = c or COLOR_BLACK
  local cells = {}
  for i = 1, w do cells[i] = px end
  local row = string.pack(string.rep("I4", w), table.unpack(cells))
  local rows = {}
  for i = 1, h do rows[i] = row end
  return setmetatable({ w = w, h = h, data = table.concat(rows) }, image)
end

function image.new(a, b, c)
  if type(a) == "table" then
    local w = math.max(1, math.floor(a.w or a.width or 1))
    local h = math.max(1, math.floor(a.h or a.height or 1))
    return setmetatable({ w = w, h = h, data = a.data }, image)
  end
  if type(a) == "string" then
    error("ti: image.new(TI.Image string) is not supported -- the Nspire's "
      .. "compressed image format has no decoder here", 2)
  end
  return image_blank(a or 1, b or 1, c)
end

function image.width(img) return img.w end
function image.height(img) return img.h end

function image.getPixel(img, x, y)
  if x < 0 or y < 0 or x >= img.w or y >= img.h then return nil end
  return string.unpack("<I4", img.data, (y * img.w + x) * 4 + 1)
end

function image.setPixel(img, x, y, c)
  if x < 0 or y < 0 or x >= img.w or y >= img.h then return end
  local at = (y * img.w + x) * 4 + 1
  img.data = img.data:sub(1, at - 1)
    .. string.pack("<I4", c or COLOR_BLACK) .. img.data:sub(at + 4)
end

-- nearest-neighbour rescale, row by row (the packed row format makes this cheap)
function image.copy(img, w, h)
  w = math.max(1, math.floor(w or img.w))
  h = math.max(1, math.floor(h or img.h))
  local rows = {}
  for y = 0, h - 1 do
    local sy = math.floor(y * img.h / h)
    local src = img.data:sub(sy * img.w * 4 + 1, (sy + 1) * img.w * 4)
    local row = {}
    for x = 1, w do
      local sx = math.floor((x - 1) * img.w / w) * 4 + 1
      row[x] = src:sub(sx, sx + 3)
    end
    rows[y + 1] = table.concat(row)
  end
  return setmetatable({ w = w, h = h, data = table.concat(rows) }, image)
end

function image.rotate(img, angle)
  angle = ((angle or 0) % 360)
  if angle == 0 then return image.copy(img, img.w, img.h) end
  local w, h = img.w, img.h
  local out_w, out_h = w, h
  if angle == 90 or angle == 270 then out_w, out_h = h, w end
  local out = image_blank(out_w, out_h, COLOR_BLACK)
  local rad = math.rad(angle)
  local cos, sin = math.cos(rad), math.sin(rad)
  local cx, cy = (w - 1) / 2, (h - 1) / 2
  local ox, oy = (out_w - 1) / 2, (out_h - 1) / 2
  for y = 0, out_h - 1 do
    for x = 0, out_w - 1 do
      local dx, dy = x - ox, y - oy
      local sx = math.floor(cos * dx + sin * dy + cx + 0.5)
      local sy = math.floor(-sin * dx + cos * dy + cy + 0.5)
      if sx >= 0 and sy >= 0 and sx < w and sy < h then
        local c = string.unpack("<I4", img.data, (sy * w + sx) * 4 + 1)
        local at = (y * out_w + x) * 4 + 1
        out.data = out.data:sub(1, at - 1) .. string.pack("<I4", c)
          .. out.data:sub(at + 4)
      end
    end
  end
  return out
end

M.image = image

-- ---------------------------------------------------------------------------
-- var: the document's variables, persisted to a file
-- ---------------------------------------------------------------------------
M.var_file = "ti_vars.lua"      -- where var.store persists (a module knob)

local vars = {}
local monitored = {}

local function serialize(v, indent)
  local t = type(v)
  if t == "number" or t == "boolean" then return tostring(v) end
  if t == "string" then return string.format("%q", v) end
  if t == "table" then
    local parts = {}
    for i = 1, #v do parts[#parts + 1] = serialize(v[i], indent) end
    for k, val in pairs(v) do
      if not (type(k) == "number" and k >= 1 and k <= #v and k % 1 == 0) then
        parts[#parts + 1] = string.format("[%s]=%s", serialize(k, indent),
                                          serialize(val, indent))
      end
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "nil"
end

local function vars_save()
  local f = io.open(M.var_file, "w")
  if not f then return "cannot write " .. M.var_file end
  f:write("return ")
  f:write(serialize(vars, 0))
  f:write("\n")
  f:close()
  return nil
end

local function vars_load()
  local f = io.open(M.var_file, "r")
  if not f then return end
  local text = f:read("*a")
  f:close()
  local chunk = load(text, M.var_file)
  if not chunk then return end
  local ok, t = pcall(chunk)
  if ok and type(t) == "table" then vars = t end
end

vars_load()

local var = {}

function var.store(name, data)
  name = tostring(name)
  if type(data) == "function" or type(data) == "userdata" then
    return "ti: cannot store a " .. type(data) .. " in " .. name
  end
  vars[name] = data
  local err = vars_save()
  if monitored[name] and M.on and M.on.varChange then
    pcall(M.on.varChange, name, data)
  end
  return err
end

function var.recall(name)
  return vars[tostring(name)]
end

function var.recallstr(name)
  local v = vars[tostring(name)]
  if v == nil then return nil end
  if type(v) == "string" then return v end
  return tostring(v)
end

function var.list()
  local names = {}
  for k in pairs(vars) do names[#names + 1] = k end
  table.sort(names)
  return names
end

function var.monitor(name) monitored[tostring(name)] = true end
function var.unmonitor(name) monitored[tostring(name)] = nil end
function var.ignore(name) monitored["!" .. tostring(name)] = true end
function var.unignore(name) monitored["!" .. tostring(name)] = nil end

function var.makeNumericList(name)
  if vars[tostring(name)] == nil then
    vars[tostring(name)] = {}
    vars_save()
  end
end

local function at_container(v, col, row)
  if type(v) ~= "table" then return nil end
  if row == nil then return v, col end
  local line = v[col]
  if type(line) ~= "table" then return nil end
  return line, row
end

function var.storeAt(name, value, col, row)
  name = tostring(name)
  local v = vars[name]
  if type(v) ~= "table" then v = {}; vars[name] = v end
  local container, index = at_container(v, col, row)
  if not container then
    container = {}
    v[col] = container
    index = row
  end
  container[index] = value
  return vars_save()
end

function var.recallAt(name, col, row)
  local container, index = at_container(vars[tostring(name)], col, row)
  if not container then return nil end
  return container[index]
end

M.var = var
-- primeLua extras, for tests and for scripts that want a clean slate
M.vars_reset = function() vars = {}; vars_save() end   -- empty, and save that
M.vars_forget = function() vars = {} end               -- empty, file untouched
M.vars_load = function() vars_load() end               -- read the file back

-- ---------------------------------------------------------------------------
-- timer
-- ---------------------------------------------------------------------------
local timer = { period = nil, last = 0 }

function timer.start(period)
  period = tonumber(period) or 0.5
  if period < 0.01 then period = 0.01 end        -- TI's documented minimum
  timer.period = period
  timer.last = os.clock()
end

function timer.stop()
  timer.period = nil
end

function timer.getMilliSecCounter()
  return math.floor(os.clock() * 1000)
end

function timer.getMillisecondsSinceEpoch()
  local t = os.time()
  if not t or t == 0 then return 0 end
  return t * 1000 + math.floor((os.clock() % 1) * 1000)
end

M.timer = timer

-- ---------------------------------------------------------------------------
-- platform (+ the window object)
-- ---------------------------------------------------------------------------
local window = {}
window.__index = window

local background = color.white

local function screen_w() return (gfx and gfx.width and gfx.width()) or 320 end
local function screen_h() return (gfx and gfx.height and gfx.height()) or 240 end

function window:width() return screen_w() end
function window:height() return screen_h() end
function window:invalidate() M.invalidate() end
function window:setBackgroundColor(c) background = c or color.white end
function window:setFocus(_) return true end

local platform = {
  apiLevel = M.API_LEVEL,
  window = setmetatable({}, window),
}

function platform.isColorDisplay() return true end
function platform.isDeviceModeRendering() return true end
function platform.isTabletModeRendering() return false end
function platform.hw() return 0 end                 -- 0 = handheld, TI's table
function platform.getString(key, default) return default end
function platform.withGC(fn)                        -- TI: run fn with a gc
  if type(fn) == "function" then
    fn(M.gc)
    M.invalidate()
  end
end
function platform.registerErrorHandler(fn) M.error_handler = fn end

M.platform = platform
M.window = platform.window

-- ---------------------------------------------------------------------------
-- keys: primeLua's device key codes under the Nspire's names
-- ---------------------------------------------------------------------------
local keys = {
  enter = key.ENTER, escape = key.ESC, esc = key.ESC,
  left = key.LEFT, right = key.RIGHT, up = key.UP, down = key.DOWN,
  backspace = key.BACKSPACE, delete = key.BACKSPACE,
  space = key.SPACE, tab = key.SYMB,
  help = key.HELP, menu = key.MENU, alpha = key.ALPHA, on = key.ON,
  shift = key.SHIFT, apps = key.APPS, plot = key.PLOT, num = key.NUM,
  view = key.VIEW, cas = key.CAS, symb = key.SYMB,
}
M.keys = keys

-- ---------------------------------------------------------------------------
-- locale / clipboard (the Prime gives Lua neither)
-- ---------------------------------------------------------------------------
local locale = {
  name = function() return "en" end,
  country = function() return "" end,
  isMetric = function() return true end,
}
M.locale = locale

local clipboard = {
  addText = function(_) return nil end,
  getText = function() return "" end,
  setText = function(_) return nil end,
}
M.clipboard = clipboard

-- ---------------------------------------------------------------------------
-- extended standard library
-- ---------------------------------------------------------------------------
local function extend_stdlib()
  string.split = function(s, delim)
    local out, pattern = {}, delim or "%s+"
    local pos = 1
    while true do
      local a, b = s:find(pattern, pos)
      if not a then
        out[#out + 1] = s:sub(pos)
        break
      end
      out[#out + 1] = s:sub(pos, a - 1)
      pos = b + 1
      if b < a then pos = a + 1 end
    end
    return out
  end

  string.uchar = function(...)
    local out = {}
    for _, cp in ipairs({ ... }) do
      if cp < 0x80 then
        out[#out + 1] = string.char(cp)
      elseif cp < 0x800 then
        out[#out + 1] = string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F))
      elseif cp < 0x10000 then
        out[#out + 1] = string.char(0xE0 | (cp >> 12),
                                    0x80 | ((cp >> 6) & 0x3F),
                                    0x80 | (cp & 0x3F))
      else
        out[#out + 1] = string.char(0xF0 | (cp >> 18),
                                    0x80 | ((cp >> 12) & 0x3F),
                                    0x80 | ((cp >> 6) & 0x3F),
                                    0x80 | (cp & 0x3F))
      end
    end
    return table.concat(out)
  end

  -- byte offset of every character, plus a sentinel one past the end
  local function char_starts(s)
    local at = {}
    for p in utf8.codes(s) do at[#at + 1] = p end
    at[#at + 1] = #s + 1
    return at
  end

  string.usub = function(s, i, j)
    local at = char_starts(s)
    local n = #at - 1                   -- number of characters
    i = i or 1
    j = j or -1
    if i < 0 then i = n + i + 1 end
    if j < 0 then j = n + j + 1 end
    if i < 1 then i = 1 end
    if j > n then j = n end
    if i > j or n == 0 then return "" end
    return s:sub(at[i], at[j + 1] - 1)
  end

  -- Lua's own evaluator: enough for arithmetic/string expressions, and nil for
  -- anything that needs the Nspire's CAS (documented in the header)
  local function eval_to_value(expr)
    local chunk = load("return " .. tostring(expr), "ti.math.eval")
    if not chunk then return nil end
    local ok, v = pcall(chunk)
    if ok then return v end
    return nil
  end

  math.eval = function(expr)
    if type(expr) == "number" then return expr end
    return eval_to_value(expr)
  end
  math.evalStr = function(expr)
    local v = eval_to_value(expr)
    if v == nil then return nil end
    return tostring(v)
  end
  local eval_settings = {}
  math.getEvalSettings = function() return eval_settings end
  math.setEvalSettings = function(t) eval_settings = t or {} end
end

-- ---------------------------------------------------------------------------
-- install: publish the API as globals and as module fields
-- ---------------------------------------------------------------------------
M.on = {}                     -- the event table the script fills in
M.invalid = true              -- platform.window:invalidate() sets this
M.quitting = false            -- ti.stop() sets this (NOT ti.quit: keep the
M.error_handler = nil         -- state and the action apart, they are one name)

function M.install()
  extend_stdlib()
  -- also publish the module itself as a global, the way primeLua's hardware
  -- libraries (gfx/key/sys/font) are published: a converted TI script ends with
  -- a bare `require("ti")` and then `ti.run()`, with no assignment in between
  _G.ti = M
  _G.platform = platform
  _G.on = M.on
  _G.color = color
  _G.timer = timer
  _G.var = var
  _G.keys = keys
  _G.image = image
  _G.locale = locale
  _G.clipboard = clipboard
end

function M.invalidate()
  M.invalid = true
end

function M.stop()
  M.quitting = true
end

-- ---------------------------------------------------------------------------
-- event dispatch
-- ---------------------------------------------------------------------------
local ARROW = {
  [key.LEFT] = "left", [key.RIGHT] = "right",
  [key.UP] = "up", [key.DOWN] = "down",
}
local ARROW_HANDLER = {
  left = "arrowLeft", right = "arrowRight", up = "arrowUp", down = "arrowDown",
}

local function call(name, ...)
  local h = M.on[name]
  if type(h) ~= "function" then return false end
  local ok, err = pcall(h, ...)
  if not ok then
    if M.error_handler then
      pcall(M.error_handler, err)
    else
      log_error(name, err)
    end
    M.errors = (M.errors or 0) + 1
  else
    M.errors = 0
  end
  return true
end

local function dispatch_key(kc)
  local arrow = ARROW[kc]
  if arrow then
    call(ARROW_HANDLER[arrow], arrow)
    call("arrowKey", arrow)
    return
  end
  if kc == key.ENTER then
    call("returnKey")
    call("enterKey")
  elseif kc == key.ESC then
    call("escapeKey")
    if not M.esc_stays then M.quitting = true end
  elseif kc == key.BACKSPACE then
    call("backspaceKey")
  elseif kc == key.SYMB then
    call("tabKey")
  elseif kc == key.MENU then
    call("contextMenu")
  elseif kc == key.HELP then
    call("help")
  elseif kc >= 0x20 and kc < 0x7F then
    call("charIn", string.char(kc))
  end
end

local function dispatch_touch()
  if not (key.gettouch) then return end
  local x, y = key.gettouch(0)
  if x then
    if not M.touch_down then
      M.touch_down = true
      call("mouseDown", x, y)
    else
      call("mouseMove", x, y)
    end
    M.touch_at = { x, y }
  elseif M.touch_down then
    M.touch_down = false
    local at = M.touch_at or { 0, 0 }
    call("mouseUp", at[1], at[2])
  end
end

local function maybe_paint()
  if not M.invalid then return end
  M.invalid = false
  if type(M.on.paint) == "function" then
    if gfx.clear then gfx.clear(background) end
    call("paint", M.gc)
  end
end

local function maybe_timer()
  if not timer.period then return end
  local now = os.clock()
  if (now - timer.last) >= timer.period then
    timer.last = now
    call("timer")
  end
end

-- ---------------------------------------------------------------------------
-- the loop
-- ---------------------------------------------------------------------------
local DEFAULT_SECONDS = 300
local MAX_ERRORS = 20          -- a handler that keeps throwing must not loop forever

-- one iteration: poll a key, run the timer, repaint if asked.  ms = how long to
-- wait for the key (the wait is what paces the loop).
local DRAIN_MAX = 16               -- events handled per iteration, like gfxdemo

function M.step(ms)
  ms = ms or 20
  local kc = key.getkey(ms)        -- this wait is what paces the loop
  if kc then
    M.nkeys = (M.nkeys or 0) + 1
    M.events = (M.events or 0) + 1
    dispatch_key(kc)
  end
  for _ = 2, DRAIN_MAX do          -- then take whatever else arrived
    local more = key.getkey(0)
    if not more then break end
    M.nkeys = (M.nkeys or 0) + 1
    M.events = (M.events or 0) + 1
    dispatch_key(more)
  end
  local tx, ty = key.gettouch(0)
  if tx then M.ntouches = (M.ntouches or 0) + 1 end
  dispatch_touch()
  maybe_timer()
  maybe_paint()
  if (M.errors or 0) >= MAX_ERRORS then
    log_error("loop", "too many errors, stopping")
    M.quitting = true
  end
  return not M.quitting
end

-- start the event loop.  opts: {seconds = n, esc_stays = bool}
function M.run(opts)
  opts = opts or {}
  M.esc_stays = opts.esc_stays and true or false
  M.quitting = false
  M.errors = 0
  M.invalid = true
  M.ran = true

  -- The event loop POLLS the firmware input hook (key.getkey), so the hook
  -- has to be installed -- and the launcher does not do it for us: primeLua's
  -- hardware libraries are documented as "install the hook yourself", and a
  -- script that forgets gets no keys at all (which on the calculator looks
  -- exactly like "the screen is up but every key is dead").  A converted TI
  -- script never knew about any of this, so ti.run() does it.
  M.hook = false
  if key and key.install then
    M.hook = key.install() and true or false
    if not M.hook then
      -- Without the hook there is NO input at all, so running on would just
      -- look like a frozen calculator until the deadline.  Say so and leave:
      -- the fix (a reboot, or the launcher clearing the stale patch) is
      -- something the user has to do on the device.
      local msg = "ti: the input hook could NOT be installed: the firmware's "
        .. "get_event slot is patched by an older session. No key or touch can "
        .. "arrive -- reboot the calculator (or run 'u' in the launcher to "
        .. "unload the image), then try again."
      if sys and sys.log then sys.log(msg) end
      print(msg)
      M.quitting = true
      if opts.diag then M.report() end
      return true
    end
  else
    log_error("run", "no key library: this build cannot take input")
  end

  -- on.construction is the documented name; on.create is the older one (a lot
  -- of published TI scripts still use it) and is an ALIAS, not an extra event:
  -- calling both would run a script's setup twice, and the setup is where a
  -- game initialises its globals.
  if type(M.on.construction) == "function" then
    call("construction")
  else
    call("create")
  end
  call("resize", screen_w(), screen_h())
  call("activate")

  local limit = opts.seconds or DEFAULT_SECONDS
  local deadline = os.clock() + limit
  local wait = 20
  if timer.period then
    wait = math.max(5, math.min(20, math.floor(timer.period * 1000 / 2)))
  end

  M.events = 0
  while not M.quitting and os.clock() < deadline do
    M.step(wait)
  end

  call("deactivate")
  if M.quitting and M.on.destroy then call("destroy") end

  -- give the slot back: the hook's code and state live in the interpreter
  -- image, and a patch left behind after the script exits outlives it
  if M.hook then
    pcall(key.remove)
    M.hook = false
  end

  if opts.diag then M.report() end
  return M.quitting
end

-- What happened during the run, in two lines that fit the on-device log.
-- Useful when a converted script "does nothing": the commonest cause is that
-- no event ever arrived (the input hook), the second commonest is that the
-- script kept throwing (see M.errors).
function M.report()
  local out = {}
  out[#out + 1] = string.format(
    "ti: run done: quitting=%s errors=%d events=%d keys=%d touches=%d",
    tostring(M.quitting), M.errors or 0, M.events or 0, M.nkeys or 0,
    M.ntouches or 0)
  local st = key and key.touchstats and key.touchstats()
  if st then
    out[#out + 1] = string.format(
      "ti: touch: frames=%d items<=%d contacts<=%d (2 means the panel reports "
      .. "two fingers)", st.frames, st.max_items, st.max_contacts)
  end
  for _, line in ipairs(out) do
    if sys and sys.log then sys.log(line) end
    print(line)
  end
end

-- load a TI script and run it (the script only defines on.* handlers)
function M.runfile(name, opts)
  local chunk, err = loadfile(name)
  if not chunk then
    error("ti: cannot load " .. tostring(name) .. ": " .. tostring(err), 2)
  end
  chunk()
  if M.ran then return true end      -- the script called ti.run() itself
  return M.run(opts)
end

-- remove the globals again (used by tests, and kind to the next script)
function M.uninstall()
  _G.ti = nil
  _G.platform, _G.on, _G.color, _G.timer = nil, nil, nil, nil
  _G.var, _G.keys, _G.image, _G.locale, _G.clipboard = nil, nil, nil, nil, nil
end

M.install()
return M
