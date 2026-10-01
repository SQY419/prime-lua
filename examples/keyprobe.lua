--# no-compare
--# expect-exit: 0
--# max-insn: 400000000
-- keyprobe.lua -- which step of the input hook resets the calculator?
--
-- Run this when a program that uses the keyboard reboots the machine.  It does
-- nothing but the input sequence, one step at a time, and writes each step to
-- keyprobe.log BEFORE it runs and closes the file again -- so if the machine
-- resets, the LAST LINE of that file names the step that did it.  (Same
-- protocol as hwcheck.lua, one level finer: hwcheck proves the API works, this
-- localises the failure.)
--
--     install      key.install()      -- writes the trap into the firmware's
--                                        get_event slot (plus cache maintenance)
--     hooked       key.hooked()       -- reads the shared state back
--     getkey(0)    a non-blocking poll
--     sleep(20)    sys.sleep(20)      -- firmware os_sleep
--     getkey(300)  a blocking poll    -- the hook's queue, after an event
--     event        key.event()        -- the same queue, as a table
--     remove       key.remove()       -- puts the firmware's own bytes back
--
-- How to read a reset (log ends at X):
--
--   install   the slot patch itself.  Compare the CP15 operations the image
--             uses with a build that works on this machine -- the wide
--             "clean D + invalidate D + invalidate TLB" form resets the G1,
--             the "drain write buffer + invalidate I-cache" pair does not
--             (see the HP_INPUT_MINIMAL_FLUSH note in primetcc/rt/hp_input.c
--             and README section "the input hook").
--   getkey    the hook entry itself, i.e. an input EVENT going through the
--             trap.  `hooked()` still being true says the slot is patched and
--             the OS reached the state.
--   sleep     nothing to do with input: sys.sleep() is firmware os_sleep.
--   remove    the restore path (hp_input_remove).
--
-- Nothing here is destructive: it installs and removes the hook exactly like
-- gfxdemo.lua/clock.lua do.  It draws nothing, so a reset costs nothing visible.

local LOG = "keyprobe.log"

-- The log is kept IN MEMORY and the whole file is rewritten on every mark.
-- Reason: this firmware only really supports "rb" and "wb+" (see
-- port/plua_fwmode.c), and reopening with "a" -- which is what the first
-- version of this script did -- produced an EMPTY keyprobe.log on the device.
-- Rewriting with "w" every time costs nothing at this size and cannot lose
-- lines.  Everything is ALSO printed, so the launcher's console shows the same
-- trail even if the file is unreadable.
local lines = {}
local function rewrite()
  local f = io.open(LOG, "w")
  if not f then
    return
  end
  f:write(table.concat(lines, "\n") .. "\n")
  f:flush()
  f:close()
end

local function mark(what)
  lines[#lines + 1] = what
  rewrite()
  print(what)
  if sys.log then
    sys.log("keyprobe: " .. what)
  end
end

local function step(name, fn)
  mark("start  " .. name)
  local ok, res = pcall(fn)
  mark((ok and "ok     " or "FAIL   ") .. name ..
       (res ~= nil and ("  -> " .. tostring(res)) or ""))
  return ok, res
end

-- key.hookstats() is the whole input path in 16 numbers: which layer stopped
-- (armed / slot / calls), what it saw, and whether the last slot restore was
-- verified by reading the firmware's bytes back.  Log it after every step.
local function stats(what)
  if not key.hookstats then
    mark("stats  (no key.hookstats in this lua.elf)")
    return
  end
  local s = key.hookstats()
  mark(string.format(
    "stats  %s: armed=%d slot=%d calls=%d unarmed_drop=%d bad=%d " ..
    "keys=%d/%d touch=%d/%d moves=%d qfull=%d inhook=%d " ..
    "restore=%d verified=%d restore_bad=%d installs=%d",
    what, s.armed, s.slot, s.calls, s.dropped_unarmed, s.bad_type,
    s.key_press, s.key_release, s.touch_press, s.touch_release,
    s.touch_moves, s.touch_jitter, s.queue_full, s.touch_frames,
    s.restore_writes, s.restore_verified, s.restore_failed, s.installs))
end

mark("---- keyprobe start ----")
mark("version " .. tostring(sys.version and sys.version() or "?") ..
     ", hooked before = " .. tostring(key.hooked()))
stats("before install")

step("install", function() return key.install() end)
stats("after install")
step("hooked", function() return key.hooked() end)
step("getkey(0)", function() return tostring(key.getkey(0)) end)
step("sleep(20)", function() sys.sleep(20); return "slept" end)
-- 3 s, announced: a 300 ms window cannot be met by a human, so "no key" said
-- nothing about whether the hook is being CALLED at all.  hookstats().calls
-- is the number that matters here -- it counts the OS entering our entry.
print("press any key now (3 s) ...")
step("getkey(3000)", function() return tostring(key.getkey(3000)) end)
mark("press again for the blocking wait ...")
print("press any key now (2 s more) ...")
step("getkey(2000)", function() return tostring(key.getkey(2000)) end)
step("event", function()
  local e = key.event()
  return e and tostring(e.type) or "none"
end)
step("remove", function() key.remove(); return key.hooked() end)
stats("after remove")

mark("---- keyprobe end ----")
print("keyprobe: done -- see " .. LOG)
print("keyprobe: if the machine reset, the last line of " .. LOG ..
      " is the step that did it")
