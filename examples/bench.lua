-- bench.lua -- how much does the launcher's watching cost the program?
--
-- Run it twice and compare:
--   1. normally (streaming: the launcher polls the ring while this runs),
--   2. with an EMPTY file named "nothread" in the app folder (the old
--      synchronous mode: the launcher is blocked and does not poll at all).
-- The two numbers os.clock() reports are the interpreter's real seconds, so the
-- difference IS the streaming tax.  On the device it should be a few percent;
-- if it is tens of percent, the pacing constants in calc/main.py
-- (STREAM_POLL_MS / STREAM_POLL_IDLE_MS) are asking for more polls than this
-- machine can spare.
--
-- 200k iterations is ~a second here: long enough to measure, short enough to
-- press ON if you want to see the interrupt instead.

local WORK = 200000

io.write(string.format("bench: %d iterations ... ", WORK))
local t0 = os.clock()
local s = 0
for i = 1, WORK do
  s = s + (i % 7) * 3 - 1
end
local dt = os.clock() - t0

print(string.format("done in %.3f s (os.clock), checksum %d", dt, s))
print(string.format("interp version %s, %s clock", sys.version(),
                    sys.time() and "firmware" or "launcher-epoch"))
