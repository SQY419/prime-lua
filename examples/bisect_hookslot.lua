--# no-compare
--# expect-exit: 0
--# max-insn: 900000000
-- bisect_hookslot.lua -- ONLY the slot write: key.install() then key.remove(), nothing in between.
--
-- Split off from bisect_keys.lua, which is the ONE step that kills input while
-- every other primitive (nothing / clock-without-keys / os.time / gfx / sleep)
-- is clean.  Its stats say the hook itself worked (calls=37, keys=4/4,
-- restore=1 verified=1, slot back to the firmware's bytes), so the damage is
-- either the SLOT WRITE itself or our code RUNNING ON THE OS'S INPUT THREAD.
-- These two scripts separate exactly those:
--
--   bisect_hookslot.lua   install + remove immediately.  The OS input thread
--                         almost certainly never enters the hook (calls = 0),
--                         so what is left is the 16-byte slot write and the
--                         cache maintenance around it.
--   bisect_hookentry.lua  install + 3 s of waiting, so the OS definitely calls
--                         the hook (calls > 0), then remove.  If THIS one kills
--                         input and the slot-only one does not, the entry --
--                         our code on the input thread -- is the culprit.
--
-- HOW TO USE: pick it, let it finish, leave the program, TOUCH THE SCREEN.
-- Dead input -> this file is the culprit.  Reboot before the next step.

local LOG = "bisect_hookslot.log"

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
  if sys.log then sys.log("hookslot: " .. what) end
end

local function stats(what)
  local s = key.hookstats and key.hookstats()
  if not s then
    mark("(no key.hookstats in this lua.elf)")
    return
  end
  mark(string.format(
    "%s: armed=%d slot=%d calls=%d unarmed=%d bad=%d keys=%d/%d touch=%d/%d " ..
    "moves=%d jitter=%d qfull=%d frames=%d restore=%d verified=%d restore_bad=%d",
    what, s.armed, s.slot, s.calls, s.dropped_unarmed, s.bad_type,
    s.key_press, s.key_release, s.touch_press, s.touch_release,
    s.touch_moves, s.touch_jitter, s.queue_full, s.touch_frames,
    s.restore_writes, s.restore_verified, s.restore_failed))
end

mark("---- bisect_hookslot.lua ----")
mark("version " .. tostring(sys.version and sys.version() or "?"))
local t0 = os.time()

stats("before")
mark("install -> " .. tostring(key.install()))
stats("installed")
mark("hooked  -> " .. tostring(key.hooked()))
key.remove()
stats("removed")
mark("no waiting, no polling: the OS had no chance to enter the hook")

mark(string.format("---- bisect_hookslot.lua done, %d s ----", os.time() - (t0 or 0)))
print("bisect_hookslot.lua: done -- NOW LEAVE THE PROGRAM AND TOUCH THE SCREEN")
