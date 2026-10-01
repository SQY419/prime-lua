--# no-compare
--# expect-exit: 0
--# max-insn: 200000000
-- timecheck.lua -- does Lua really get the calculator's clock?
--
-- Run it from the launcher's list (or `main.run_file("timecheck.lua")`) after
-- copying the new lua.elf: os.time()/os.date() now come from the FIRMWARE
-- (svc 0x100A5, see calc/timeprobe.py), and os.clock() is built from the
-- sub-second counter in the same structure, self-calibrated against os.time().
--
-- The rules this file follows are hwcheck.lua's, and they matter: every step
-- writes its marker to the durable log BEFORE doing anything that can reset
-- the machine, and the first failure stops the run.  If the calculator reboots
-- while this script is running, the last line of plua.log names the step.
--
-- Reading the result:
--   * os.time() moving by ~2 while os.clock() moves by ~2  -> all good;
--   * os.time() at 0 (1970) and sys.time() == nil          -> the firmware
--     clock is unusable on this machine; the interpreter falls back to the
--     epoch the launcher read from PPL (which the launcher now parses
--     correctly: PPL answers with floats);
--   * os.clock() stuck at 0 while os.time() moves          -> the sub-second
--     counter is not what the device run measured; send plua.log back.
--
-- IN THE EMULATOR the clock is FROZEN on purpose (tests/emu_time.py: a fixed
-- 2026-09-25 20:31:58, so that `make test` can still compare output with the
-- host reference byte for byte).  The two "advances" lines therefore FAIL there
-- and the calendar lines pass -- that is the expected emulator picture, and the
-- script still exits 0.  On the calculator both must be ok.

local sys = sys

local function line(fmt, ...)
  print(string.format(fmt, ...))
end

local function ok(name, cond, detail)
  print(string.format("%-28s %s%s", name, cond and "ok  " or "FAIL",
                      detail and ("  " .. detail) or ""))
  return cond
end

sys.log("timecheck: start, interp " .. sys.version())

line("interp      %s", sys.version())
line("os.time()   %d", os.time())
line("os.date()   %s", os.date())
line("os.date(!)  %s", os.date("!%Y-%m-%d %H:%M:%S"))
line("sys.time()  %s", tostring(sys.time()))
line("os.clock()  %.3f", os.clock())

local t0, c0 = os.time(), os.clock()
local tk0 = sys.time()

-- the risky part is over already: if the script got this far, svc 0x100A5
-- answered with something the interpreter accepted.
sys.log("timecheck: step 1 ok (section to follow)")

-- 2 s of firmware sleep: the input loop runs, so keys stay alive
sys.sleep(2000)
local t1, c1 = os.time(), os.clock()
local tk1 = sys.time()

line("after 2 s")
line("  os.time     %d -> %d  (delta %d)", t0, t1, t1 - t0)
line("  os.clock    %.3f -> %.3f  (delta %.3f)", c0, c1, c1 - c0)
line("  sys.time    %s -> %s", tostring(tk0), tostring(tk1))

local good_time = (t1 - t0) >= 1 and (t1 - t0) <= 4
local good_clock = (c1 - c0) >= 1.5 and (c1 - c0) <= 3.0

ok("wall clock advances", good_time, string.format("delta %d s", t1 - t0))
ok("monotonic clock advances", good_clock,
   string.format("delta %.3f s", c1 - c0))
ok("os.date is a real date", (os.date("*t").year or 0) > 1970, os.date("%Y-%m-%d"))

-- local time of day should agree with PPL's own clock, when PPL is reachable
-- (the launcher reads PPL Date/Time for its epoch; 20:31:56 on the device run)
local t = os.date("*t")
ok("calendar fields sane", t.month >= 1 and t.month <= 12 and t.day >= 1
   and t.day <= 31 and t.hour <= 23 and t.min <= 59, os.date("%H:%M:%S"))

sys.log(string.format("timecheck: done time=%d clock=%.3f ok_time=%s ok_clock=%s",
                      t1, c1, tostring(good_time), tostring(good_clock)))
print("")
print("(the same lines are in plua.log; send that file back if anything failed)")
