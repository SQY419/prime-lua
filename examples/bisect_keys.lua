--# no-compare
--# expect-exit: 0
--# max-insn: 900000000
-- bisect_keys.lua -- ONE primitive, then exit.  the input hook only: install, 150 getkey(200) polls, remove.
--
-- HOW TO USE (same for every bisect_*.lua):
--   1. pick it in the list, let it finish (it prints as it goes)
--   2. leave the program (any key / ESC)
--   3. TOUCH THE SCREEN
--   4. keys+ touch dead?  -> this step is the culprit, tell me the file name.
--      still fine?         -> this primitive is exonerated.
--   5. reboot before the next step when a step did kill input.
--
-- WHY THESE EXIST: after a program that uses the keyboard exits, touching the
-- screen on this machine can leave keys and touch dead, and the calculator
-- resets.  The input hook is provably clean by then -- the launcher log shows
-- after every run:
--
--     [hook] stand-down: armed=0 (state 0x...)
--     [hook] slot now: E92D0001 E92D4000 (firmware stub)
--
-- so the damage is done by something the program does WHILE it runs.  Each of
-- these scripts does exactly one of the candidates, for the same wall time.
--
-- bisect_none.lua   nothing at all -- THE CONTROL. If the machine dies after
--                   this one too, the fragility has nothing to do with us.
-- bisect_clock.lua  what clock.lua does, minus the keyboard: gfx + os.time in
--                   a loop for ~20 s.  The most direct test of "is the input
--                   hook involved at all".
-- bisect_time.lua   os.time() in a loop (svc 0x100A5, the firmware clock).
-- bisect_draw.lua   gfx only: full-screen fill + text, ~600 frames.
-- bisect_keys.lua   the input hook only: install, poll getkey(200) x 150,
--                   remove.
-- bisect_sleep.lua  sys.sleep(20) x 600 (already reported clean -- kept so the
--                   matrix is complete).

local LOG = "bisect_keys.log"

-- The log is kept in memory and rewritten with "w" on every mark: this
-- firmware only supports "rb"/"wb+" reliably (port/plua_fwmode.c), and the
-- earlier "a"-mode version wrote an EMPTY file on the device.  Everything is
-- printed as well, so the launcher's console shows the same trail.
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
  if sys.log then sys.log("keys: " .. what) end
end

mark("---- bisect_keys.lua ----")
mark("version " .. tostring(sys.version and sys.version() or "?"))
local t0 = os.time()

mark("install -> " .. tostring(key.install()))
mark("hooked  -> " .. tostring(key.hooked()))
local got, TICKS = 0, 150
-- exactly the pattern clock.lua uses: one blocking poll per tick, spending its
-- time inside sys.sleep(5) -> firmware os_sleep
for i = 1, TICKS do
  if key.getkey(200) then got = got + 1 end
end
mark("polled " .. TICKS .. " x getkey(200), " .. got .. " key(s)")
key.remove()
local s = key.hookstats and key.hookstats() or nil
if s then
  mark(string.format(
    "stats: armed=%d slot=%d calls=%d unarmed=%d bad=%d keys=%d/%d touch=%d/%d " ..
    "moves=%d jitter=%d qfull=%d frames=%d restore=%d verified=%d restore_bad=%d",
    s.armed, s.slot, s.calls, s.dropped_unarmed, s.bad_type,
    s.key_press, s.key_release, s.touch_press, s.touch_release,
    s.touch_moves, s.touch_jitter, s.queue_full, s.touch_frames,
    s.restore_writes, s.restore_verified, s.restore_failed))
end


mark(string.format("---- bisect_keys.lua done, %d s ----", os.time() - (t0 or 0)))
print("bisect_keys.lua: done -- NOW LEAVE THE PROGRAM AND TOUCH THE SCREEN")
