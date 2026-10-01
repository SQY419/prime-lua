"""
svcprobe.py -- which NON-BLOCKING input services does this machine actually have?

WHY THIS EXISTS
    primeLua reads the keyboard by patching the firmware's get_event slot and
    observing the events the OS fetches.  That works, but on the real calculator
    a program that used the hook leaves the machine's input path damaged: after
    it exits, the first touch kills keys and touch, and the calculator resets.
    The hook itself is provably clean by then -- the launcher log shows, after
    every run,

        [hook] stand-down: armed=0 (state 0x...)
        [hook] slot now: E92D0001 E92D4000 (firmware stub)

    -- so the damage is done while our code is ON the OS's input thread, which
    is a context we do not control.

    The way out is to stop intercepting: poll the kernel instead.  The firmware
    image in this workspace (PRIME_APP.DAT -> PROGRAMS/MISC/ARMFIR.ELF) has a
    service stub table at 0x307FBF1C, 12 bytes per entry, svc 0x10000+n, and the
    SDKLIB export map names these candidates:

        0x10034 GetSysKeyState     0x10040 GetPendEvent    0x10046 TestPendEvent
        0x1003b GetPenEvent        0x10042 GetEventType    0x10047 ClearPendEvent
        0x1003c CheckPenEvent      0x10045 GetLastEvent    0x1004b TestKeyEvent
        0x1003d ClearPenEvent

    Every one of them is a QUERY, and a query is what a polling input layer
    needs.  What is NOT known is their ABI -- and guessing an ABI on this
    machine can reset it (that is exactly what svc 0x100A5 did when primetcc
    called it with r0 = 0).  So this probe measures instead of guessing, the
    same way calc/timeprobe.py did for the clock:

      * one service per step, in increasing order of risk (no-argument queries
        first, then the ones that plausibly take a buffer);
      * every step is written to svcprobe.log and the file is CLOSED before the
        call, so if the calculator resets, the LAST LINE names the call that
        did it;
      * after each call the machine is asked to prove it is alive (PPL Ticks),
        and the return value in r0 plus any memory the call wrote is recorded;
      * nothing is called twice.

USAGE ON THE CALCULATOR (Python app):

    import svcprobe
    svcprobe.run()             # safe steps only: no service is called
    svcprobe.run(risky=True)   # the calls above, one at a time
    svcprobe.read()            # the log, as a list of lines
    svcprobe.reset()           # empty the log before a clean run

    >>> import svcprobe; svcprobe.run(risky=True)

    The first run may reset the calculator at some step.  That is information,
    not an accident: reboot, then run it again -- the steps that already
    answered are in the log and are skipped.
"""

import uio
import sys

try:
    import struct                      # CPython (used by the host tests)
except ImportError:                    # MicroPython on the calculator
    import ustruct as struct

import hpprime

PROBE_VERSION = "0.2"

LOG = "svcprobe.log"

# The firmware's service stub table: entry n is at TABLE + 12*n, and each entry
# is `push {r0}; push {lr}; svc 0x10000+n`.  Verified twice, because getting it
# wrong means calling a DIFFERENT service than the one intended -- which on this
# machine is how calculators get reset:
#   * the firmware image in this workspace (PRIME_APP.DAT ->
#     PROGRAMS/MISC/ARMFIR.ELF) disassembles to `svc 0x10034` at 0x307FBF1C and
#     `svc 0x10037` at 0x307FBF40, i.e. TABLE = 0x307FBCAC;
#   * calc/timeprobe.py uses the same constant, and its device log recorded
#     "stub 0x307FC468" for svc 0x100A5 -- which is exactly TABLE + 12*0xA5.
# The slot primeLua patches, 0x307FBFA0, is entry 0x3F: get_event.
SERVICE_TABLE = 0x307FBCAC

# svc numbers (from the SDKLIB export map)
ADDR_MALLOC = None            # filled in below
ADDR_FREE = None


def svc_addr(ident):
    return SERVICE_TABLE + 0xC * (ident - 0x10000)


ADDR_MALLOC = svc_addr(0x10037)
ADDR_FREE = svc_addr(0x1003A)


class Debug:
    """The same debug-channel protocol calc/main.py uses: a binary pipe to the
    firmware (`uio.FileIO("debug")`) carrying read/write/call requests."""

    def __init__(self):
        self.f = uio.FileIO("debug")

    def write_mem(self, addr, val):
        self.f.write(struct.pack("<III", 1, addr, val))

    def read_mem(self, addr, size):
        self.f.write(struct.pack("<III", 0, addr, 0))
        return self.f.read(size)

    def call(self, func_addr, *args):
        fmt = "<III" + "I" * len(args)
        buf = bytearray(struct.calcsize(fmt))
        struct.pack_into(fmt, buf, 0, 2, func_addr, len(args), *args)
        self.f.write(buf)
        return struct.unpack_from("<I", buf, 0)[0]


# ===========================================================================
# the durable log: rewrite-with-close.  This firmware's file API only supports
# "rb"/"wb+" reliably -- an "a" handle wrote an EMPTY file in an earlier probe
# -- so the whole file is rewritten and closed on every line.  A reset cannot
# lose a closed line.
# ===========================================================================
_LINES = []


def _log(line, kind="out"):
    _LINES.append(line)
    try:
        with uio.FileIO(LOG, "wb") as f:
            f.write(("\n".join(_LINES[-400:]) + "\n").encode())
    except Exception:
        pass
    try:
        if kind == "warn":
            print("[!] " + line)
        else:
            print(line)
    except Exception:
        pass


def read():
    try:
        with uio.FileIO(LOG, "rb") as f:
            d = f.read(16384)
        if isinstance(d, bytes):
            d = d.decode("latin1")
        for ln in d.split("\n"):
            if ln and ln not in _LINES:
                _LINES.append(ln)
        return d.split("\n")
    except Exception:
        return list(_LINES)


def reset():
    del _LINES[:]
    try:
        with uio.FileIO(LOG, "wb") as f:
            f.write(b"")
    except Exception:
        pass


def _ticks():
    """PPL's millisecond counter -- the machine's own 'I am still here'."""
    try:
        return int(hpprime.eval("Ticks"))
    except Exception:
        return None


def _step_state():
    """(answered, crashed) step names from the log.

    A step whose "calling ..." line is there but whose "r0=" line is not was
    attempted and never came back: that is a RESET, and it is recorded so the
    next run does not repeat it by accident.  One reset must buy exactly one
    fact."""
    answered, crashed = set(), set()
    for line in read():
        if line.startswith("  -> calling "):
            parts = line.split()
            if len(parts) > 3:
                crashed.add(parts[3])
        elif line.startswith("  -> ") and "r0=" in line:
            nm = line.split(":", 1)[0][5:].split(" ")[0].strip()
            crashed.discard(nm)
            answered.add(nm)
    return answered, crashed


# ===========================================================================
# the steps.  A 64-byte zeroed buffer we own is handed to the calls that
# plausibly want an output buffer; reading it back says what was written.
# ===========================================================================
STEPS = [
    # ORDER MATTERS.  The first attempt called TestPendEvent with r0 = 0 and the
    # calculator RESET -- the same signature as svc 0x100A5 with r0 = 0, which
    # turned out to be a function that writes through r0.  So: every call now
    # gets a REAL 64-byte buffer we own, and the "give me the pending event"
    # calls go first, because an event buffer is what they most plausibly want.
    #
    #   name              svc       arg0
    ("GetPendEvent",     0x10040, "buf"),
    ("GetLastEvent",     0x10045, "buf"),
    ("GetPenEvent",      0x1003b, "buf"),
    ("CheckPenEvent",    0x1003c, "buf"),
    ("TestPendEvent",    0x10046, "buf"),
    ("TestKeyEvent",     0x1004b, "buf"),
    ("GetEventType",     0x10042, "buf"),
    ("GetSysKeyState",   0x10034, "buf"),
]


def stage_safe():
    _log("[i] safe stage: nothing is called, this only reports the environment")
    _log("  python     : %r" % (sys.implementation,))
    _log("  Ticks      : %s" % (_ticks(),))
    try:
        _log("  Date/Time  : %s %s" % (hpprime.eval("Date"),
                                       hpprime.eval("Time")))
    except Exception as e:
        _log("  Date/Time  : FAIL %r" % (e,))
    _log("  table 0x%08X = svc 0x10000, 0x%08X = svc 0x1003F (get_event slot)"
         % (SERVICE_TABLE, svc_addr(0x1003F)))


def stage_risky(max_calls=1):
    """Call the candidate query services -- ONE per run by default.

    Every call is preceded by a written-and-CLOSED log line, so a reset leaves
    the name of the call that caused it as the last line of svcprobe.log; the
    next run reads that and skips it.  One reset, one fact.
    """
    try:
        dbg = Debug()
    except Exception as e:
        _log("[!] no debug channel (%r) -- cannot call services" % (e,), "warn")
        return

    answered, crashed = _step_state()
    if crashed:
        _log("[i] steps that already reset the machine: %s"
             % ", ".join(sorted(crashed)))
    calls = 0
    for name, ident, arg in STEPS:
        if name in answered:
            _log("[i] %s: already answered, skipping" % name)
            continue
        if name in crashed:
            _log("[i] %s: RESET the machine last time, skipping" % name)
            continue
        if calls >= max_calls:
            _log("[i] stopping after %d call(s) -- run again for the next step"
                 % calls)
            break
        calls += 1
        addr = svc_addr(ident)
        buf = dbg.call(ADDR_MALLOC, 64)
        if not buf:
            _log("[!] malloc failed -- stopping", "warn")
            return
        for i in range(0, 64, 4):
            dbg.write_mem(buf + i, 0)
        a0 = buf if arg == "buf" else 0
        before = _ticks()
        _log("  -> calling %-16s svc 0x%05X stub 0x%08X r0=0x%08X (r1=r2=r3=0)"
             % (name, ident, addr, a0))
        _log("  -> IF THE CALCULATOR RESETS NOW, IT WAS %s (svc 0x%05X)"
             % (name, ident))
        r = dbg.call(addr, a0, 0, 0, 0)
        after = _ticks()
        wr = dbg.read_mem(buf, 32)
        _log("  -> %-16s r0=0x%08X (%d)  ticks %s->%s" % (name, r, r, before, after))
        _log("  -> %-16s buffer: %s"
             % (name, " ".join("%02X" % b for b in (wr or b"")[:32])))
        dbg.call(ADDR_FREE, buf)
        if before is not None and after is not None and after - before > 5000:
            _log("[!] %s took %d ms -- the machine was wedged for a moment"
                 % (name, after - before), "warn")


# ===========================================================================
# WATCH MODE: call ONE service in a loop while a human presses keys / touches
# the screen.
#
# A query service returns "nothing pending" at a quiet moment, so a single call
# proves nothing -- the first probe of all eight services returned 0 with an
# untouched buffer, which only says they are safe to call.  What a polling
# input layer needs to know is: WHICH service changes when input happens, and
# HOW (return value? buffer contents? which offsets?).
#
#     svcprobe.watch("TestKeyEvent")      # then press keys for 10 s
#     svcprobe.watch("GetPendEvent")      # press keys AND touch
#     svcprobe.watch("GetSysKeyState")    # press keys
#     svcprobe.watch("CheckPenEvent")     # touch the screen
#     svcprobe.watch("GetPenEvent")       # touch the screen
#     svcprobe.watch("GetLastEvent")      # anything
#
# Every call gets a freshly zeroed 64-byte buffer.  Each call is compared with
# the previous one: non-zero r0, or buffer words that changed, are what the log
# keeps (plus the first three examples, byte for byte).
# ===========================================================================
BY_NAME = dict((nm, (ident, arg)) for nm, ident, arg in STEPS)


def watch(name, seconds=10, period_ms=60, buf_arg=0, a0=0, a1=0, a2=0, a3=0):
    """Call ONE service in a loop and record EVERY distinct answer.

    buf_arg says WHICH argument register gets our 64-byte buffer (0 = r0, 1 =
    r1, ...).  That matters: with the buffer in r0, GetSysKeyState answered
    (it returned the pointer and wrote 0x00030032), while TestKeyEvent,
    GetPendEvent, CheckPenEvent and GetPenEvent said nothing at all -- which is
    equally consistent with "wrong argument position" as with "wrong service".

        svcprobe.watch("GetSysKeyState")            # then HOLD a key down
        svcprobe.watch("GetPendEvent", buf_arg=1)   # then press keys
        svcprobe.watch("GetPenEvent",  buf_arg=1)   # then touch the screen
        svcprobe.watch("CheckPenEvent", buf_arg=1)  # then touch
        svcprobe.watch("TestKeyEvent", buf_arg=1)   # then press keys

    Every call gets a freshly zeroed buffer, and the log keeps each DISTINCT
    (r0, buffer) pair with how often it appeared, so "it does react, but only to
    this one thing" is visible instead of being averaged away.
    """
    ident, _arg = BY_NAME.get(name, (None, None))
    if ident is None:
        _log("[!] unknown service %r -- use one of: %s"
             % (name, ", ".join(BY_NAME)), "warn")
        return
    try:
        dbg = Debug()
    except Exception as e:
        _log("[!] no debug channel (%r)" % (e,), "warn")
        return
    addr = svc_addr(ident)
    buf = dbg.call(ADDR_MALLOC, 64)
    if not buf:
        _log("[!] malloc failed", "warn")
        return

    args = [a0, a1, a2, a3]
    args[buf_arg] = buf
    # r0 IS DEREFERENCED BY THESE SERVICES.  Measured the hard way: TestPendEvent
    # with r0 = 0 and GetPenEvent with r0 = 0 both RESET the calculator, exactly
    # like svc 0x100A5 with r0 = 0 back when the clock was being worked out.  So
    # r0 is never left at 0 here, whatever register the buffer went into.
    if args[0] == 0:
        args[0] = buf if buf_arg != 0 else dbg.call(ADDR_MALLOC, 64)
    _log("==== watch %s (svc 0x%05X) for %d s -- PRESS KEYS / TOUCH THE SCREEN "
         "NOW ====" % (name, ident, seconds))
    _log("     arguments: r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X "
         "(buffer in r%d, zeroed before every call; r0 is never 0 -- these "
         "services dereference it)"
         % (args[0], args[1], args[2], args[3], buf_arg))

    seen = {}          # (r0, buf bytes) -> count
    order = []
    calls = 0
    start = _ticks()
    if start is None:
        _log("[!] PPL Ticks unavailable -- cannot time the loop", "warn")
    while start is not None:
        now = _ticks()
        if now is None or now - start >= seconds * 1000:
            break
        for i in range(0, 64, 4):
            dbg.write_mem(buf + i, 0)
        r = dbg.call(addr, args[0], args[1], args[2], args[3])
        wr = bytes(dbg.read_mem(buf, 64) or b"")
        calls += 1
        sig = (r, wr)
        if sig not in seen:
            seen[sig] = 0
            order.append(sig)
        seen[sig] += 1
        if period_ms:
            _sleep_ms(period_ms)

    _log("  -> %s: %d call(s), %d distinct answer(s)" % (name, calls, len(seen)))
    for (r, wr) in order[:6]:
        _log("     r0=0x%08X (%d) x%-4d buf=%s"
             % (r, r, seen[(r, wr)], " ".join("%02X" % b for b in wr[:32])))
    if len(seen) == 1 and order and order[0][0] == 0 and \
            not any(order[0][1]):
        _log("     nothing moved: try another argument position "
             "(buf_arg=1/2/3) or another service")
    elif len(seen) > 1:
        _log("     *** IT REACTS: %d different answers -- the one that appears "
             "while you press keys / touch is the input state" % len(seen))
    dbg.call(ADDR_FREE, buf)
    if args[0] != buf and args[0]:
        dbg.call(ADDR_FREE, args[0])
    _log("==== watch %s done ====" % name)


# ===========================================================================
# RAM DIFF: find the keyboard state in memory, with no service calls at all.
#
# WHY THIS IS THE PROMISING ONE.  The two firmware routes are dead ends on this
# machine: intercepting get_event damages the input path (bisect_hookentry.lua),
# and the query services answer nothing, because the OS's own input thread is
# sitting in get_event and takes every event before an outside poller can see it.
#
# But the keys ARE somewhere: the calculator's own PPL sees them (the launcher's
# menu is driven by hpprime.keyboard()), and PPL can only be reading memory.  So:
# read a region, have the human hold a key, read it again, and report every word
# that changed.  If a word flips with the keys, the interpreter can POLL MEMORY
# -- pure reads, no firmware call, nothing to break.
#
#     svcprobe.find_key_state()                       # 0x30000000, 512 KB
#     svcprobe.find_key_state(0x31000000, 0x00100000) # another region
#
# The scan is slow (64 bytes per debug call), so it reads a word-aligned window
# in chunks and keeps only the words that differ between the two passes.
# ===========================================================================
CHUNK = 64


def _snapshot(dbg, start, size, chunk=CHUNK):
    out = []
    for off in range(0, size, chunk):
        d = dbg.read_mem(start + off, chunk)
        out.append(bytes(d) if d else b"\x00" * chunk)
    return out


# svc numbers for the event-type mask (SDKLIB export map)
SET_EVENT_TYPE = 0x10041
GET_EVENT_TYPE = 0x10042


def event_mask(dbg, set_bit=0x20):
    """Read, open, and report the firmware's event-type mask.

    THE DISASSEMBLY OF THE FIRMWARE'S OWN EVENT LOOP IS WHY THIS EXISTS.  At
    0x30607098 the OS does exactly this before it ever calls GetEvent:

        bl GetEventType        ; svc 0x10042
        orr r0, r0, #0x20      ; <- one more event class
        bl SetEventType        ; svc 0x10041
        ... loop: GetEvent(ev) ; svc 0x1003F, blocking

    Every query service primeLua probed returned "nothing pending" -- which is
    equally consistent with "the mask says I do not want that class of event".
    So: read the mask, open the same bit the OS opens, keep the old value for
    restoring, and then poll with the mask in place.

    Returns (old_mask, new_mask).
    """
    gt, st = svc_addr(GET_EVENT_TYPE), svc_addr(SET_EVENT_TYPE)
    old = dbg.call(gt, 0, 0, 0, 0)
    new = old | set_bit
    dbg.call(st, new, 0, 0, 0)
    back = dbg.call(gt, 0, 0, 0, 0)
    _log("  mask: GetEventType=0x%08X -> SetEventType(0x%08X|0x%02X)=0x%08X "
         "(reads back 0x%08X)" % (old, old, set_bit, new, back))
    return old, new


def watch_masked(name, seconds=10, period_ms=60, buf_arg=0, set_bit=0x20):
    """watch() with the firmware's event-type mask opened first.

        svcprobe.watch_masked("GetPendEvent")   # press keys / touch
        svcprobe.watch_masked("TestKeyEvent", set_bit=0x40)
        svcprobe.watch_masked("GetPendEvent", set_bit=0x04)

    The mask bit is the one variable left: the OS opens 0x20 for its own loop,
    but which bit (if any) means "deliver key events to me" is not known yet.
    Try 0x20 first, then 0x40, 0x04, 0x01, 0x8000 -- one run each.
    """
    try:
        dbg = Debug()
    except Exception as e:
        _log("[!] no debug channel (%r)" % (e,), "warn")
        return
    _log("==== watch_masked %s (set_bit=0x%02X) ====" % (name, set_bit))
    old, new = event_mask(dbg, set_bit)
    watch(name, seconds=seconds, period_ms=period_ms, buf_arg=buf_arg)
    try:
        dbg.call(svc_addr(SET_EVENT_TYPE), old, 0, 0, 0)
        _log("  mask restored to 0x%08X" % old)
    except Exception:
        pass
    _log("==== watch_masked %s done ====" % name)


def find_key_state(start=0x30000000, size=0x00100000, chunk=256,
                   hold_ms=6000, quiet_ms=1500):
    """Find the keyboard state in RAM: three passes, no service calls.

    WHY THIS IS THE LAST ROAD THAT LEADS SOMEWHERE.  Static analysis is done:
    the firmware's own GETKEY is a modal UI wait (0x30AADE6C, next to the
    "Waiting for a keystroke" string), and the Python-visible `keyboard`/`get_key`
    names appear ONLY in a sorted name-index table (0x30CC10A4 / 0x30CC0F68) --
    no static module table references them, so the C implementation sits behind
    frozen (probably compressed) bytecode and cannot be reached by name.

    But the keys ARE in memory: the calculator's own PPL reads them, and PPL can
    only read RAM.  So find the RAM word that moves when a key is held:

        pass A  idle
        pass B  idle, immediately after   -> words that differ A/B are NOISE
        pass C  while YOU HOLD A KEY      -> candidates are words that differ
                                             B/C but NOT A/B

    The three-pass filter is what makes this usable: the machine has timers,
    counters and stacks everywhere, and a two-pass diff drowns in them.

        svcprobe.find_key_state()                         # 0x30000000, 1 MB
        svcprobe.find_key_state(0x30100000, 0x00100000)   # next megabyte
        svcprobe.find_key_state(0x30500000, 0x00100000)   # app/heap area

    Reads only.  Nothing is written, no service is called.
    """
    try:
        dbg = Debug()
    except Exception as e:
        _log("[!] no debug channel (%r)" % (e,), "warn")
        return
    _log("==== find_key_state 0x%08X..0x%08X (%d KB, %d B per read) ===="
         % (start, start + size, size // 1024, chunk))
    _log("  pass A: idle ...")
    a = _snapshot(dbg, start, size, chunk)
    _sleep_ms(quiet_ms)
    _log("  pass B: idle again (any word that moved is noise) ...")
    b = _snapshot(dbg, start, size, chunk)
    _log("  >>> NOW HOLD A KEY DOWN AND KEEP HOLDING IT (%.0f s) <<<"
         % (hold_ms / 1000.0))
    _sleep_ms(hold_ms)
    _log("  pass C: reading while the key is HELD ...")
    c = _snapshot(dbg, start, size, chunk)
    _log("  pass C done.")
    noise = 0
    hits = []
    for i, (x, y, z) in enumerate(zip(a, b, c)):
        base = start + i * chunk
        for j in range(0, min(len(x), len(y), len(z)), 4):
            if x[j:j+4] != y[j:j+4]:
                noise += 1
            elif y[j:j+4] != z[j:j+4]:
                hits.append((base + j, y[j:j+4], z[j:j+4]))
    _log("  -> noise words (moved with nothing pressed): %d" % noise)
    _log("  -> %d candidate word(s) that moved ONLY while the key was held" % len(hits))
    for addr, y, z in hits[:40]:
        _log("     0x%08X: %s -> %s"
             % (addr, " ".join("%02X" % b for b in y), " ".join("%02X" % b for b in z)))
    if hits:
        _log("  -> run it again with a DIFFERENT key held: the address that moves")
        _log("     both times, with a different value, is the keyboard state.")
    _log("==== find_key_state done ====")


def ppl_probe():
    """Which read-only PPL expressions does this machine actually have?

    PPL is the one input path that is PROVEN to work on this machine (the
    launcher's own menu is driven by hpprime.keyboard()).  If PPL also exposes a
    memory peek, then the whole hunt becomes fast: Python can scan RAM through
    PPL instead of one debug call per 256 bytes, and correlate it with
    keyboard() directly.
    """
    exprs = ["GETKEY", "Ticks", "memory(1)", "Date", "Time",
             "PEEK(0)", "PEEK(0,1)", "PEEKB(0)", "PEEKW(0)",
             "BITAND(1,1)", "TYPE(1)", "keyboard()"]
    _log("==== ppl_probe: which PPL expressions work ====")
    for ex in exprs:
        try:
            r = hpprime.eval(ex)
            _log("  %-14s -> %r" % (ex, r))
        except Exception as e:
            _log("  %-14s -> FAIL %r" % (ex, e))
    _log("==== ppl_probe done ====")


def _sleep_ms(ms):
    """Sleep through the firmware (svc 0x10008) -- proven safe by
    bisect_sleep.lua, and the same call the demos use between frames."""
    try:
        import hpprime as _h
        _h.eval("WAIT(%.3f)" % (ms / 1000.0))
        return
    except Exception:
        pass
    try:
        import time
        time.sleep(ms / 1000.0)
    except Exception:
        pass


def run(risky=False, max_calls=1):
    read()                      # pick up what an earlier (possibly reset) run wrote
    _log("==== svcprobe v%s (%s) ===="
         % (PROBE_VERSION, "RISKY: query services included" if risky
            else "safe"), "log")
    stage_safe()
    if risky:
        stage_risky(max_calls)
    else:
        _log("[i] no service was called.  run(risky=True) when you are ready.")
    _log("==== done (%s) ====" % ("risky" if risky else "safe"), "log")
