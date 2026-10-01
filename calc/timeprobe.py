"""
timeprobe.py -- which time source does THIS calculator actually offer?

WHY THIS EXISTS
    The launcher (calc/main.py) can hand lua.elf only the wall clock it reads
    ONCE at start-up, because nothing inside lua.elf can ask the firmware for
    the time: svc 0x100A5 -- "GetSysTime" in the app-side SDK -- reset the G1
    when primetcc called it with r0 = 0, and it is the ONLY time service the
    app API has (see ../sdklib_svc_map.txt: 0x100A5 GetSysTime / 0x100A6
    SetSysTime / 0x1009A Delay / 0x10008 OSSleep; there is no tick service).
    Fixing primeLua's os.time()/os.clock() therefore needs ONE measurement
    from the machine, which is what this probe takes:

      1. does the FIRMWARE's MicroPython expose utime --
         ticks_ms/ticks_us/ticks_cpu/time/localtime?  The firmware image
         contains utime_mphal.o and its qstr pool holds every one of those
         names, but "import time" fails on the device, so the module name has
         to be measured rather than assumed (utime is MicroPython's real name;
         time is only an optional alias);
      2. does svc 0x100A5 work when r0 points at a REAL buffer?  SDKLIB's
         export is literally

             push {r0}; push {lr}; svc 0x100A5

         i.e. a one-argument function -- the same shape primetcc's
         hp_svc_gettime has -- so the reboot looks like a NULL-pointer write
         (f(r0) storing into *r0) rather than a wrong service number;
      3. how PPL's Date/Time/Ticks behave (they are known to work: the
         launcher's epoch comes from them).

USAGE ON THE CALCULATOR (Python app):

    import timeprobe
    timeprobe.run()               # safe: utime + PPL only, cannot reset
    timeprobe.run(risky=True)     # + the svc 0x100A5 buffer probe

    timeprobe.read()              # the log, as a list of lines
    timeprobe.reset()             # empty the log before a clean run

    >>> import timeprobe
    >>> timeprobe.run(risky=True)

`run(risky=True)` is the ONLY thing in this file that can reset the machine,
and it follows hwcheck.lua's protocol: every marker is written to
timeprobe.log and the file CLOSED *before* the risky call, so if the calculator
resets, the last line of that file names the culprit ("calling 0x100A5 ..."
with no line after it = the call itself did it).  Results also go to the
console through moreprint when it is there.

DELIBERATELY NOT PROBED
    * svc 0x100A6 SetSysTime -- it WRITES the clock;
    * the r0 = NULL form of 0x100A5 -- that is the form known to reset.

WHAT THE FIRST DEVICE RUN (2026-09-25, v0.1) ALREADY SETTLED
    * `utime`, `time` and `umachine` are NOT importable (only `utimeq` is), so
      the firmware's utime_mphal.o is not reachable from the Python app;
    * PPL answers, and with FLOATS: Date 2026.0925 (yyyy.mmdd) and Time
      20.5322222222 (DECIMAL HOURS = 20:31:56); Ticks is milliseconds since
      boot (delta 1008 for a 1 s wait).  The launcher used to subscript those
      floats as if they were {y,m,d}/{h,m,s} lists, so its epoch was always -1
      -- fixed in calc/main.py (split_ppl_date/split_ppl_time);
    * **svc 0x100A5 WORKS with a real buffer**: r0 came back as the year (2026)
      and the 64-byte buffer was filled -- the primetcc reset was r0 = 0 being
      written through, not a wrong service number;
    * the buffer, read as little-endian u16s, held
      [year, month, day-of-week, day, hour, minute, second, ?, tail-lo,
       tail-hi] -- 2026-09-25 (dow = 5 = Friday) 20:31:58, two seconds after the
      PPL reading of the same run, plus a u32 counter at offset 16.

WHAT v0.2 ADDS
    * the sampling ladder: 6 samples 0.25 s apart plus one 3 s step, each
      carrying PPL's Ticks as an independent clock, so the per-field DELTAS say
      which word is the second, which is the sub-second counter and in which
      unit (`tail/wall = 1.000 units per ms` means milliseconds);
    * a guard on the firmware malloc result.  v0.1's reuse of the launcher's
      debug handle returned the record's op byte (2) instead of r0, and the
      probe then wrote 4 bytes to 0x00000002 on the real calculator.  A fresh
      handle plus `_plausible_heap_ptr()` makes that impossible.

NEXT STEP PER OUTCOME
    layout confirmed -> wire it into port/plua_time.c through a new
        hp_svc_gettime wrapper in port/plua_svc.s: time() reads the u16 fields
        (1-second cache), clock() uses the offset-16 counter once its unit is
        known; sys.time()/sys.ticks() in port/plua_hp.c; bump PLUA_VERSION.
    "no line after 'calling'" (a reset)       -> the Prime's kernel does not
        implement 0x100A5 at all (the 742-entry stub area is shared across
        Besta-OS machines, so a stub can exist without an implementation);
        fall back to `utime` on the launcher side plus a thread/mailbox clock,
        which is a launcher change, not a lua.elf one.
"""

PROBE_VERSION = "0.2"

import sys
import uio
import ustruct as struct

try:
    import hpprime
except Exception:                     # host / no hpprime: stay importable
    hpprime = None

# Colored console output, same import calc/main.py uses (moreprint.py is
# user-provided).  Missing module = everything still runs, just plain.
try:
    from moreprint import p_log, p_warning, p_error, p_pass, p_out
except Exception:
    def p_log(text):
        print(text)

    def p_warning(text):
        print(text)

    def p_error(text):
        print(text)

    def p_pass(text):
        print(text)

    def p_out(text):
        print(text)

LOG = "timeprobe.log"            # relative: MicroPython cannot open C:\... paths

# firmware service table: entry for service N sits at BASE + 12*(N - 0x10000)
# (the same table calc/main.py and primetcc's SVC_STUB macro use)
SERVICE_TABLE = 0x307FBCAC


def svc_addr(ident):
    return SERVICE_TABLE + 0xC * (ident - 0x10000)


ADDR_MALLOC = svc_addr(0x10037)
ADDR_FREE = svc_addr(0x1003A)
ADDR_GETTIME = svc_addr(0x100A5)     # SDKLIB's GetSysTime stub

# The sampling ladder of the risky stage: how many samples, and how long to
# wait between them.  The gaps must be short enough that a sub-second counter
# does NOT wrap between samples (otherwise its deltas say nothing) and long
# enough that the wait itself is measurable next to PPL's millisecond Ticks.
SAMPLES = 6
GAP = 0.25
LONG_GAP = 3


# ===========================================================================
# the durable log: every line is appended, then the file is CLOSED, so a reset
# cannot lose it (MicroPython's handles are not flushed when the box dies)
# ===========================================================================
def _log(line, kind="out"):
    try:
        old = b""
        try:
            with uio.FileIO(LOG, "rb") as f:
                old = f.read()
        except Exception:
            old = b""
        if len(old) > 8192:                  # keep the tail only
            cut = old.rfind(b"\n", 0, len(old) - 4096)
            old = old[cut + 1:] if cut >= 0 else old[-4096:]
        with uio.FileIO(LOG, "wb") as f:
            f.write(old + (line + "\n").encode("utf-8"))
    except Exception:
        pass
    try:
        {"log": p_log, "warn": p_warning,
         "err": p_error, "pass": p_pass}.get(kind, p_out)(line)
    except Exception:
        pass
    return line


def read():
    """The probe log as a list of lines ('' if there is none yet)."""
    try:
        with uio.FileIO(LOG, "rb") as f:
            data = f.read()
    except Exception:
        return []
    try:
        text = data.decode("utf-8")
    except Exception:
        text = "".join(chr(b) for b in data)
    return [ln for ln in text.replace("\r", "").split("\n") if ln]


def reset():
    """Empty the log, so one run's lines can be read without the previous
    run's noise (the launcher's own logs accumulate on purpose; this one is a
    single measurement)."""
    try:
        with uio.FileIO(LOG, "wb") as f:
            f.write(b"")
        return True
    except Exception as e:
        _log("cannot reset %s: %r" % (LOG, e), "warn")
        return False


# ===========================================================================
# the debug channel (a private copy of calc/main.py's PrimeDebug)
#
# A copy rather than `import main`, because importing the launcher on the
# device starts its screen selector.  The protocol is three 32-bit words per
# record: op 0 read / 1 write / 2 call, and a call leaves its result in the
# caller's buffer -- that is how PrimeDebug.call() gets a value out of a
# "write" (mem.malloc depends on it).
# ===========================================================================
class _Debug:
    def __init__(self, filename="debug"):
        self.f = uio.FileIO(filename)

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass

    def read_mem(self, addr, size):
        self.f.write(struct.pack("<III", 0, addr, 0))
        return self.f.read(size)

    def write_mem(self, addr, val):
        self.f.write(struct.pack("<III", 1, addr, val))

    def write_mem_bytes(self, addr, data):
        pad = (4 - (len(data) % 4)) % 4
        data = bytes(data) + b"\x00" * pad
        for i in range(0, len(data), 4):
            self.write_mem(addr + i, struct.unpack("<I", data[i:i + 4])[0])

    def call(self, func_addr, *args):
        fmt = "<III" + "I" * len(args)
        buf = bytearray(struct.calcsize(fmt))
        struct.pack_into(fmt, buf, 0, 2, func_addr, len(args), *args)
        self.f.write(buf)
        return struct.unpack_from("<I", buf, 0)[0]


def _open_debug():
    """A debug handle for this probe, and whether we own it.

    ALWAYS a fresh handle.  Reusing the launcher's (v0.1 did, when `main`
    happened to be importable) turned out to be WRONG and dangerous: in the
    2025-09-25 device run the launcher's handle returned the record's op byte
    (2) instead of the callee's r0, so the probe took "the malloc result" to be
    2 and wrote 4 bytes to address 0x00000002 on the real calculator.  A fresh
    `uio.FileIO("debug")` returns the true r0 (the same run: r0 = 2026 from
    GetSysTime), which is also how calc/main.py's own handle behaves.
    """
    return _Debug("debug"), True, "a fresh 'debug' handle"


# On the device the firmware heap answers with 0x30xxxxxx (the 2025-09-25 run:
# 0x305B7830).  The emulator's application memory starts at 0x31000000, so the
# window is wide enough to cover both; what matters is that it excludes 0, 2
# and every other small number a confused handle can hand back.
HEAP_LO, HEAP_HI = 0x30000000, 0x32000000


def _plausible_heap_ptr(a):
    """A firmware-heap pointer, or something we must NOT dereference.

    The check that would have caught the v0.1 accident before it wrote
    anywhere: the interpreter image, the output ring and every launcher
    allocation live above 0x30000000, and a 4-byte-aligned address below that
    is either garbage or -- as in a reset case -- address 0 itself."""
    return isinstance(a, int) and a & 3 == 0 and HEAP_LO <= a < HEAP_HI


def _delay(seconds):
    """Wait, preferring MicroPython's own sleep (it also lets the OS run its
    input loop).  Returns the string form of what actually did the waiting, so
    the log says how trustworthy the interval is."""
    ms = int(seconds * 1000)
    try:
        import utime
        utime.sleep_ms(ms)
        return "utime.sleep_ms(%d)" % ms, True
    except Exception:
        pass
    if hpprime is not None:
        try:
            hpprime.eval("wait(%d)" % int(seconds))
            return "PPL wait(%d)" % int(seconds), True
        except Exception:
            pass
    n = 0
    for i in range(int(seconds * 100000)):      # rough, and said to be rough
        n += i
    return "busy loop (rough)", False


# ===========================================================================
# stage 1: what the firmware's MicroPython exposes
# ===========================================================================
_TIME_NAMES = ("utime", "time", "umachine", "utimeq")


def stage_env():
    _log("-- environment --------------------------------------------", "log")
    try:
        _log("  sys.implementation: %r"
             % (getattr(getattr(sys, "implementation", None), "name", "?"),))
    except Exception:
        pass
    for name in _TIME_NAMES:
        try:
            m = __import__(name)
            names = []
            for attr in ("time", "localtime", "gmtime", "mktime", "sleep",
                         "sleep_ms", "sleep_us", "ticks_ms", "ticks_us",
                         "ticks_cpu", "ticks_add", "ticks_diff",
                         "time_pulse_us"):
                try:
                    getattr(m, attr)
                    names.append(attr)
                except Exception:
                    pass
            _log("  import %-9s ok    %s" % (name, ",".join(names) or "(no time names)"),
                 "pass")
        except Exception as e:
            _log("  import %-9s FAIL  %r" % (name, e), "warn")


def stage_utime():
    """ticks_* need a unit check: sample, wait ~1 s, sample again.  delta ~1000
    means milliseconds (what os.clock() wants); delta ~1 means seconds; a
    constant means the function is a stub."""
    _log("-- MicroPython utime -------------------------------------", "log")
    try:
        import utime
    except Exception as e:
        _log("  no utime module (%r): ticks_ms/time cannot be used here" % (e,),
             "warn")
        return None

    out = {}
    for attr in ("ticks_ms", "ticks_us", "ticks_cpu"):
        try:
            f = getattr(utime, attr)
        except Exception:
            continue
        try:
            a = f()
            how, _exact = _delay(1)
            b = f()
            d = b - a
            unit = _guess_unit(d)
            out[attr] = d
            _log("  %-10s %d -> %d  delta %d  %s  (waited: %s)"
                 % (attr, a, b, d, unit, how),
                 "pass" if unit.startswith("ms") or unit.startswith("us") else "warn")
        except Exception as e:
            _log("  %-10s FAIL %r" % (attr, e), "warn")

    for attr in ("time", "localtime", "gmtime"):
        try:
            f = getattr(utime, attr)
        except Exception:
            continue
        try:
            a = f()
            how, _exact = _delay(2)
            b = f()
            _log("  %-10s %r -> %r  (delta %r, waited: %s)"
                 % (attr, a, b, _delta(a, b), how), "pass")
        except Exception as e:
            _log("  %-10s FAIL %r" % (attr, e), "warn")
    return out


def _delta(a, b):
    try:
        return b - a
    except Exception:
        return "?"


def _guess_unit(d):
    if d == 0:
        return "no advance at all (stub or frozen)"
    if 900 <= d <= 1100:
        return "milliseconds per tick"
    if 900000 <= d <= 1100000:
        return "microseconds per tick"
    if 1 <= d <= 3:
        return "SECONDS per tick"
    return "unknown unit (delta %d for a ~1 s wait)" % d


# ===========================================================================
# stage 2: PPL (known to work -- the launcher's epoch comes from here)
# ===========================================================================
def stage_ppl():
    _log("-- PPL (hpprime.eval) ------------------------------------", "log")
    if hpprime is None:
        _log("  no hpprime module: not on a calculator?", "warn")
        return None
    vals = {}
    for expr in ("Date", "Time", "Ticks", "memory(1)"):
        try:
            vals[expr] = hpprime.eval(expr)
            _log("  %-10s -> %r" % (expr, vals[expr]), "pass")
        except Exception as e:
            _log("  %-10s FAIL %r" % (expr, e), "warn")
    try:
        a = hpprime.eval("Ticks")
        how, _exact = _delay(1)
        b = hpprime.eval("Ticks")
        try:
            d = b - a
            _log("  Ticks      %r -> %r  delta %r  %s  (waited: %s)"
                 % (a, b, d, _guess_unit(d), how), "pass")
        except Exception:
            _log("  Ticks      %r -> %r  (not a number?, waited: %s)"
                 % (a, b, how), "warn")
    except Exception as e:
        _log("  Ticks      sampling FAIL %r" % (e,), "warn")
    return vals


# ===========================================================================
# stage 3 (RISKY): svc 0x100A5 with a real buffer
# ===========================================================================
def _words(data):
    out = []
    for i in range(0, len(data) - 3, 4):
        out.append(struct.unpack_from("<I", data, i)[0])
    return out


def _halfwords(data):
    out = []
    for i in range(0, len(data) - 1, 2):
        out.append(struct.unpack_from("<H", data, i)[0])
    return out


def _fields(hw):
    """The layout the FIRST device run (2025-09-25) measured, as little-endian
    u16s:

        0  year        2  month      4  day-of-week (0 = Sunday)
        6  day         8  hour      10  minute
       12  second     14  ?         16  sub-second / uptime counter (u32)

    Checked against PPL at the time of that run: Date 2026.0925 and
    Time 20.5322 (decimal hours = 20:31:56) with the buffer reading
    2026-09-25 (Friday, dow = 5) 20:31:58 -- 2 s later, i.e. the same clock.
    Returns None when the buffer does not look like that at all."""
    if len(hw) < 10:
        return None
    f = {"year": hw[0], "month": hw[1], "dow": hw[2], "day": hw[3],
         "hour": hw[4], "minute": hw[5], "second": hw[6], "f14": hw[7],
         "tail": hw[8] | (hw[9] << 16)}
    if not (1970 <= f["year"] <= 2200 and 1 <= f["month"] <= 12
            and f["dow"] <= 6 and 1 <= f["day"] <= 31 and f["hour"] <= 23
            and f["minute"] <= 59 and f["second"] <= 60):
        return None
    return f


def _decode(hw):
    f = _fields(hw)
    if f is None:
        return "no known layout (raw halfwords above)"
    return ("%04d-%02d-%02d (dow=%d) %02d:%02d:%02d  f14=%d tail=%d"
            % (f["year"], f["month"], f["day"], f["dow"], f["hour"],
               f["minute"], f["second"], f["f14"], f["tail"]))


def _looks_like_time(r):
    return 946684800 <= r <= 4102444800


def _ticks():
    """PPL's Ticks (ms since boot), the independent clock the deltas are
    measured against -- or None when PPL will not answer."""
    if hpprime is None:
        return None
    try:
        return int(hpprime.eval("Ticks"))
    except Exception:
        return None


def _sample(dbg, buf):
    r = dbg.call(ADDR_GETTIME, buf, 0, 0, 0)
    hw = _halfwords(dbg.read_mem(buf, 32))
    return r, hw, _ticks()


def stage_svc_gettime():
    """THE risky step.  Everything is logged and closed before the call.

    Once the first call has come back (proving this machine implements the
    service), it is sampled repeatedly -- each sample carrying PPL's Ticks as
    an independent clock -- because the per-field DELTAS are what say which
    word is the second, which is a sub-second counter, and in which unit.
    """
    _log("-- svc 0x100A5 GetSysTime (RISKY) ------------------------", "log")
    try:
        dbg, owned, source = _open_debug()
    except Exception as e:
        _log("  cannot open the debug device (%r): skipping" % (e,), "warn")
        return None
    _log("  using %s" % (source,))

    buf = 0
    try:
        buf = dbg.call(ADDR_MALLOC, 64)
    except Exception as e:
        _log("  firmware malloc FAIL %r: skipping" % (e,), "err")
        return None
    # NEVER dereference a pointer the firmware did not hand us: a handle that
    # answers with the wrong register turns "buf" into 2, and writing there is
    # a poke at address 0x00000002 on the real machine.
    if not _plausible_heap_ptr(buf):
        _log("  malloc returned 0x%08X, which is not a firmware-heap pointer:"
             " refusing to write through it" % (buf & 0xFFFFFFFF), "err")
        return None

    try:
        _log("  stub 0x%08X, buffer 0x%08X (64 B, zeroed)"
             % (ADDR_GETTIME, buf))
        dbg.write_mem_bytes(buf, b"\x00" * 64)

        # ---- the risky call; this line is the last one if the box resets ----
        _log("  calling 0x100A5(buf, 0, 0, 0) ...")
        try:
            r = dbg.call(ADDR_GETTIME, buf, 0, 0, 0)
        except Exception as e:
            _log("  call raised %r -- on hardware this is the reset" % (e,),
                 "err")
            return None
        _log("  0x100A5 returned r0 = 0x%08X (%d)" % (r, r), "pass")

        data = dbg.read_mem(buf, 32)
        words = _words(data)
        hw = _halfwords(data)
        _log("  buf words: " + " ".join("%08X" % w for w in words))
        _log("  buf halves: " + " ".join("%04X" % h for h in hw[:12]))
        _log("  buf untouched (all zero): GetSysTime takes no output buffer"
             if not any(words) else "  buf got data")
        _log("  decode: " + _decode(hw))
        if any(words):
            _log("  -> SIGNATURE: gettime(SYSTIME *) writes through r0", "pass")
        elif _looks_like_time(r):
            _log("  -> SIGNATURE: gettime(void) -> unix seconds in r0", "pass")
        else:
            _log("  -> neither a buffer nor a plausible timestamp: "
                 "inconclusive", "warn")

        # ---- the sampling ladder: deltas per field, against PPL Ticks ----
        samples = [(r, hw, _ticks())]
        for k in range(1, SAMPLES):
            _delay(GAP)
            samples.append(_sample(dbg, buf))
        _log("  sampling every %g s (PPL Ticks is the reference clock):" % GAP)
        for k, (rr, h, tk) in enumerate(samples):
            f = _fields(h)
            _log("    t%d r0=%-6d ticks=%-8s %s"
                 % (k, rr, tk if tk is not None else "?",
                    ("%02d:%02d:%02d f14=%-5d tail=%-9d"
                     % (f["hour"], f["minute"], f["second"], f["f14"],
                        f["tail"])) if f else "(no layout)"))
        _log("  per-step deltas (field index, u16 view):")
        for k in range(len(samples) - 1):
            a, b = samples[k][1], samples[k + 1][1]
            d1, d2 = samples[k][2], samples[k + 1][2]
            ch = " ".join("%d:%+d" % (j, b[j] - a[j])
                          for j in range(min(len(a), len(b)))
                          if a[j] != b[j])
            ref = (d2 - d1) if (d1 is not None and d2 is not None) else None
            _log("    t%d->t%d  dTicks=%-6s  %s"
                 % (k, k + 1, ref if ref is not None else "?", ch or "(none)"))

        # ---- one long step: the second field must gain exactly the wall time
        how, _exact = _delay(LONG_GAP)
        r3, hw3, tk3 = _sample(dbg, buf)
        f0, f3 = _fields(samples[0][1]), _fields(hw3)
        _log("  after a %s: r0=%d ticks=%s" % (how, r3, tk3))
        _log("  decode: " + _decode(hw3))
        if f0 and f3:
            dt = samples[0][2]
            wall = (tk3 - dt) if (dt is not None and tk3 is not None) else None
            _log("    seconds %d -> %d, wall %s, tail %d -> %d, f14 %d -> %d"
                 % (f0["second"], f3["second"], wall, f0["tail"], f3["tail"],
                    f0["f14"], f3["f14"]))
            if wall:
                dtail = f3["tail"] - f0["tail"]
                _log("    tail/wall = %.3f units per ms  (1.000 = ms, "
                     "1.024 = 1/1024 s)" % (dtail / float(wall),))
        return r
    finally:
        try:
            dbg.call(ADDR_FREE, buf)
        except Exception:
            pass
        if owned:
            try:
                dbg.close()
            except Exception:
                pass


# ===========================================================================
# driver
# ===========================================================================
def run(risky=False):
    """Take the measurement.  Safe stages only unless risky=True."""
    _log("==== timeprobe v%s (%s) ===="
         % (PROBE_VERSION, "RISKY: svc 0x100A5 included" if risky else "safe"),
         "log")
    _log("[i] facts this probe is testing against: SDKLIB GetSysTime = "
         "'push {r0}; push {lr}; svc 0x100A5'; firmware contains utime_mphal.o",
         "log")
    stage_env()
    stage_utime()
    stage_ppl()
    if risky:
        stage_svc_gettime()
    else:
        _log("[i] svc 0x100A5 NOT probed -- it can reset the calculator:",
             "warn")
        _log("[i]   run(risky=True) when you are ready for that", "warn")
    _log("==== done (%s) ====" % ("risky" if risky else "safe"), "log")
    return read()
