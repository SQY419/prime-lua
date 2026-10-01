--# no-compare
--# expect-exit: 0
--# max-insn: 300000000
-- hookprobe.lua -- is the firmware's get_event stub safe to call from the
-- resident input hook?
--
-- THE QUESTION THIS ANSWERS.  primeLua's input hook sits in the firmware's
-- get_event slot and, to get the event the firmware just fetched, it calls the
-- ORIGINAL 16 bytes of that slot (the "trampoline").  That call runs on the
-- firmware's OWN input thread.  If the firmware's stub blocks when its queue is
-- empty, the hook blocks too -- and the calculator stops answering its keys,
-- touches and menus: the screen stays where it was, one event may have gone
-- through, and nothing else ever arrives.
--
-- The symptom that sends people here: "the first key/touch did something, then
-- everything froze".
--
-- WHAT IT DOES.  Installs the hook and then, once a second, logs a line with a
-- counter and any key it got.  Read the log (sys.log writes plua.log, which
-- survives a reboot):
--
--   * the counter KEEPS GROWING and keys appear  -> the stub is safe; the bug
--     is elsewhere (the hook's queue, or the script).
--   * the counter STOPS after the first line      -> the firmware's thread is
--     blocked inside our hook: the stub must not be called.  That is the
--     finding this script exists for.
--   * the counter never starts                    -> the hook never fired at
--     all (slot not patched, or the OS is not polling through it).
--
-- It exits on ESC, or after 20 seconds, whichever comes first.

local gfx, key, sys = gfx, key, sys

local function log(s)
  if sys and sys.log then sys.log("hookprobe: " .. s) end
  print("hookprobe: " .. s)
end

log("start, install=" .. tostring(key.install()))
log("hooked=" .. tostring(key.hooked()))

local function seconds_since_start()
  return os.clock()
end

local t0 = seconds_since_start()
local tick, keys, touches = 0, 0, 0
local quit = false

while not quit and (seconds_since_start() - t0) < 20 do
  local ev = key.event()
  while ev do
    if ev.type == "key" then
      keys = keys + 1
      log(string.format("key code=%d down=%s", ev.code, tostring(ev.down)))
      if ev.code == key.ESC then quit = true end
    elseif ev.type == "touch" then
      touches = touches + 1
    end
    ev = key.event()
  end
  if (seconds_since_start() - t0) >= (tick + 1) then
    tick = tick + 1
    local h = key.hookstats()
    log(string.format("tick %d: keys=%d touches=%d contacts=%d | "
                      .. "hook armed=%s calls=%d pushed=%d queued=%d "
                      .. "head=%d tail=%d | L2proc=%d badtype=%d "
                      .. "kpress=%d k28=%d k30=%d k32=%d k34=%d "
                      .. "seen_key=%d seen_touch=%d last_type=%d last_down=%d "
                      .. "last_key=%d last_dw=%08x",
                      tick, keys, touches, select(2, key.contacts()),
                      tostring(h.armed), h.calls, h.pushed, h.queued,
                      h.head, h.tail, h.L2proc, h.badtype,
                      h.kpress, h.k28, h.k30, h.k32, h.k34,
                      h.seen_key, h.seen_touch, h.last_type, h.last_down,
                      h.last_key, h.last_dw))
  end
  sys.sleep(50)
end

log(string.format("done after %d tick(s), keys=%d touches=%d", tick, keys,
                  touches))
local st = key.touchstats()
log(string.format("touch frames=%d max items=%d max contacts=%d expired=%d",
                  st.frames, st.max_items, st.max_contacts, st.expired))
key.remove()
log("hook removed")
