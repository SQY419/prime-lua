--[[----------------------------------------------------------------------------
  zoom.lua -- the view transform behind examples/imggesture.lua.

  A view is one small table:  v = { s = scale, x = , y = }  where (x, y) is
  where the image's ORIGIN (its top-left pixel) lands on the screen.  Image
  pixel (ix, iy) is drawn at (x + ix*s, y + iy*s).

      local v = zoom.fit(iw, ih, 320, 240)      -- contain-fit, centred
      v = zoom.pinch(v, ax, ay, bx, by, cx, cy, dx, dy, 0.2, 40)
      v = zoom.pan(v, dx, dy)
      v = zoom.clamp(v, iw, ih, 320, 240)
      local dx, dy, dw, dh, sx, sy, sw, sh = zoom.visible(v, iw, ih, 320, 240)
      img.strblitpart(pic, dx, dy, dw, dh, sx, sy, sw, sh)

  `zoom.visible` is the part that makes a zoomed image cheap to draw: it returns
  the destination rectangle INTERSECTED with the viewport plus the matching
  source rectangle, so a 10x zoom costs 320x240 pixel writes per frame instead
  of 10*10*320*240, and the pixels outside the screen are never touched (the
  firmware's blitter would drop them one by one).

  All of it is pure arithmetic -- no drawing, no hardware -- which is why
  tests/zoom_host.lua can check the invariants exactly on the host, including
  the one that matters for a pinch: the image point under the fingers stays
  under the fingers.
----------------------------------------------------------------------------]]

local M = {}

M.MIN_SCALE = 0.05
M.MAX_SCALE = 40

local function clampf(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- contain-fit, centred, with optional padding in screen pixels
function M.fit(iw, ih, sw, sh, pad)
  pad = pad or 0
  local s = math.min((sw - 2 * pad) / iw, (sh - 2 * pad) / ih)
  if s <= 0 then s = M.MIN_SCALE end
  return { s = s,
           x = (sw - iw * s) / 2,
           y = (sh - ih * s) / 2 }
end

function M.pan(v, dx, dy)
  return { s = v.s, x = v.x + dx, y = v.y + dy }
end

local function dist(ax, ay, bx, by)
  local dx, dy = bx - ax, by - ay
  return math.sqrt(dx * dx + dy * dy)
end

-- two-finger pinch: (a,b) are the fingers before, (c,d) after.  The scale
-- follows the distance ratio and the image point that was under the midpoint
-- stays under the new midpoint -- which also gives two-finger panning for free,
-- because a drag moves the midpoint without changing the distance.
function M.pinch(v, ax, ay, bx, by, cx, cy, dx, dy, mins, maxs)
  local d0 = dist(ax, ay, bx, by)
  local d1 = dist(cx, cy, dx, dy)
  local m0x, m0y = (ax + bx) / 2, (ay + by) / 2
  local m1x, m1y = (cx + dx) / 2, (cy + dy) / 2
  local s = v.s
  if d0 > 0.001 and d1 > 0.001 then
    s = clampf(v.s * (d1 / d0), mins or M.MIN_SCALE, maxs or M.MAX_SCALE)
  end
  -- the image coordinate under the old midpoint
  local ix = (m0x - v.x) / v.s
  local iy = (m0y - v.y) / v.s
  return { s = s, x = m1x - ix * s, y = m1y - iy * s }
end

-- keep the image where it can be seen: when it is larger than the viewport,
-- clamp so no empty border shows; when it is smaller, centre it
function M.clamp(v, iw, ih, sw, sh)
  local s = clampf(v.s, M.MIN_SCALE, M.MAX_SCALE)
  local w, h = iw * s, ih * s
  local x, y
  if w <= sw then x = (sw - w) / 2
  else x = clampf(v.x, sw - w, 0) end
  if h <= sh then y = (sh - h) / 2
  else y = clampf(v.y, sh - h, 0) end
  return { s = s, x = x, y = y }
end

-- destination rect (clipped to the viewport) and the source rect it samples
function M.visible(v, iw, ih, sw, sh)
  local x0 = math.max(0, math.floor(v.x))
  local y0 = math.max(0, math.floor(v.y))
  local x1 = math.min(sw, math.ceil(v.x + iw * v.s))
  local y1 = math.min(sh, math.ceil(v.y + ih * v.s))
  if x1 <= x0 or y1 <= y0 then return nil end
  local sx = math.floor((x0 - v.x) / v.s)
  local sy = math.floor((y0 - v.y) / v.s)
  local swid = math.min(iw - sx, math.ceil((x1 - x0) / v.s))
  local shei = math.min(ih - sy, math.ceil((y1 - y0) / v.s))
  if sx < 0 then sx = 0 end
  if sy < 0 then sy = 0 end
  if swid < 1 then swid = 1 end
  if shei < 1 then shei = 1 end
  if sx + swid > iw then swid = iw - sx end
  if sy + shei > ih then shei = ih - sy end
  if swid < 1 or shei < 1 then return nil end
  return x0, y0, x1 - x0, y1 - y0, sx, sy, swid, shei
end

return M
