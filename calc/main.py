"""
main.py -- primeLua: the Lua 5.4 interpreter on the HP Prime G1.

    >>> import main

Run it from the calculator's Python app: a dictionary-style list of the .lua
files in the app folder, ENTER runs the selected one, and its output is printed
while it runs (ON or ESC interrupts it).

This file is kept DELIBERATELY SHORT: the calculator compiles a module's whole
source into RAM (156 KB of source cost ~6 MB of heap), so the comments and
docstrings live in main_notes.md next to it, in the same order.  See that file
for how the loader, the input hook, the streaming console and the screen UI
work, and README.md for the project as a whole.
"""
import sys

import ustruct as struct
import uio

try:
    import hpprime
except Exception:
    hpprime = None

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

def decode_bytes(b):
    try:
        return b.decode("utf-8")
    except Exception:
        try:
            return b.decode("latin-1")
        except Exception:
            return "".join(chr(x) for x in b)

class PrimeDebug:
    def __init__(self, filename="debug"):
        try:
            self.f = uio.FileIO("debug")
        except Exception:
            self.f = open(filename, "rb")

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass

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

    def write_mem_bytes(self, addr, data):
        pad = (4 - (len(data) % 4)) % 4
        data = bytes(data) + b"\x00" * pad
        for i in range(0, len(data), 4):
            self.write_mem(addr + i,
                           struct.unpack("<I", data[i:i + 4])[0])

BASE = 0x307FBCAC

def get_addr(int_id):
    return BASE + 0xC * (int_id - 0x10000)

ADDR_MALLOC = get_addr(0x10037)
ADDR_FREE = get_addr(0x1003A)
ADDR_SLEEP = get_addr(0x10008)

W_FIND_FIRST = get_addr(0x10270)
W_FIND_NEXT = get_addr(0x10271)
FIND_CLOSE = get_addr(0x100DA)
W_GET_ATTR = get_addr(0x10272)
W_GETCWD = get_addr(0x1027A)
GET_DISK_ID = get_addr(0x100E7)

RING_SIZE = 32768

def str_to_utf16le_bytes(s):
    out = bytearray()
    for ch in s:
        val = ord(ch)
        out.append(val & 0xFF)
        out.append((val >> 8) & 0xFF)
    out.append(0)
    out.append(0)
    return bytes(out)

class MemUtils:

    def __init__(self, dbg):
        self.dbg = dbg

    def malloc(self, size):
        return self.dbg.call(ADDR_MALLOC, (size + 3) & ~3)

    def free(self, addr):
        if addr:
            self.dbg.call(ADDR_FREE, addr)

    def alloc_wstr(self, s):
        b = str_to_utf16le_bytes(s)
        addr = self.malloc(len(b))
        self.dbg.write_mem_bytes(addr, b)
        return addr

    def read_wstr(self, addr, max_chars=256):
        if not addr:
            return ""
        raw = self.dbg.read_mem(addr, max_chars * 2)
        out = []
        for i in range(0, len(raw) - 1, 2):
            c = raw[i] | (raw[i + 1] << 8)
            if c == 0:
                break
            out.append(chr(c) if 32 <= c < 0xFFFD else "?")
        return "".join(out)

    def read_utf8(self, addr, max_len=256):
        raw = self.dbg.read_mem(addr, max_len)
        end = raw.find(b"\x00")
        if end >= 0:
            raw = raw[:end]
        return decode_bytes(raw)

class ElfTools:
    @staticmethod
    def get_elf_memory_size(filename):
        required = 0
        try:
            with uio.FileIO(filename, "rb") as f:
                ehdr = f.read(52)
                if len(ehdr) < 52 or ehdr[0:4] != b"\x7fELF":
                    return 0
                e_phoff = struct.unpack_from("<I", ehdr, 28)[0]
                e_phentsize = struct.unpack_from("<H", ehdr, 42)[0]
                e_phnum = struct.unpack_from("<H", ehdr, 44)[0]
                f.seek(e_phoff)
                for _ in range(e_phnum):
                    ph = f.read(e_phentsize)
                    if struct.unpack_from("<I", ph, 0)[0] != 1:
                        continue
                    end = struct.unpack_from("<I", ph, 8)[0] + \
                        struct.unpack_from("<I", ph, 20)[0]
                    if end > required:
                        required = end
        except Exception:
            return 0
        return (required + 3) & ~3

    @staticmethod
    def file_fingerprint(filename):
        try:
            with uio.FileIO(filename, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                if size < 96:
                    return None
                h = size & 0xFFFFFFFF
                for off in (0, (size - 32) // 2, size - 32):
                    f.seek(off)
                    chunk = f.read(32)
                    if len(chunk) < 32:
                        return None
                    for i in range(32):
                        h = (h * 31 + chunk[i] + i) & 0xFFFFFFFF
                return (size, h)
        except Exception:
            return None

    @staticmethod
    def find_magic_addr(filename, magic, loaded_base):
        try:
            with uio.FileIO(filename, "rb") as f:
                ehdr = f.read(52)
                if len(ehdr) < 52 or ehdr[0:4] != b"\x7fELF":
                    return 0
                e_phoff = struct.unpack_from("<I", ehdr, 28)[0]
                e_phentsize = struct.unpack_from("<H", ehdr, 42)[0]
                e_phnum = struct.unpack_from("<H", ehdr, 44)[0]
                f.seek(e_phoff)
                segs = []
                for _ in range(e_phnum):
                    ph = f.read(e_phentsize)
                    if len(ph) < e_phentsize:
                        break
                    if struct.unpack_from("<I", ph, 0)[0] == 1:
                        segs.append((struct.unpack_from("<I", ph, 4)[0],
                                     struct.unpack_from("<I", ph, 8)[0],
                                     struct.unpack_from("<I", ph, 16)[0]))
                f.seek(0)
                pos, carry = 0, b""
                while True:
                    chunk = f.read(4096)
                    if not chunk:
                        return 0
                    window = carry + chunk
                    base_pos = pos - len(carry)
                    idx = window.find(magic)
                    if idx >= 0:
                        abs_off = base_pos + idx
                        for (p_off, p_vaddr, p_filesz) in segs:
                            if p_off <= abs_off < p_off + p_filesz:
                                return loaded_base + p_vaddr + \
                                    (abs_off - p_off)
                        return 0
                    carry = window[-7:]
                    pos += len(chunk)
        except Exception:
            return 0

LOADER_CODE = (
    b'\x0b\x00\x00\xea\x04\x00-\xe5\x04\xe0-\xe5o\x02\x01\xef\x04\x00-\xe5'
    b'\x04\xe0-\xe5\xca\x00\x01\xef\x04\x00-\xe5\x04\xe0-\xe5\xd4\x00\x01'
    b'\xef\x04\x00-\xe5\x04\xe0-\xe5\xcf\x00\x01\xefh2\x9f\xe5\xf0O-\xe9'
    b'\x030\x8f\xe0\x00 \xa0\xe1\x01`\xa0\xe1\x03\x00\x93\xe8d\xd0M\xe2'
    b'\x04\x00\x8d\xe5\xb8\x10\xcd\xe1\x02\x00\xa0\xe1\x04\x10\x8d\xe2'
    b'\xe7\xff\xff\xeb\x00@P\xe2s\x00\x00\n\x040\xa0\xe1\x01 \xa0\xe3'
    b'4\x10\xa0\xe3,\x00\x8d\xe2\xe6\xff\xff\xeb\x01\x00P\xe3j\x00\x00'
    b'\x1a\xbc"\xdd\xe1\x142\x9f\xe5\x03\x00R\xe1f\x00\x00\x1aH\x10\x9d'
    b'\xe5\x00 \xa0\xe3\x04\x00\xa0\xe1\xdf\xff\xff\xeb\xb85\xdd\xe1'
    b'\x00\x00S\xe3x\x00\x00\n\x00P\xa0\xe3\x05p\xa0\xe1\x0c\xa0\x8d'
    b'\xe2\x04\x00\x00\xea\xb85\xdd\xe1\x02\x00[\xe3\x14p\x9d\x05\x05'
    b'\x00S\xe1\x19\x00\x00\xda\x040\xa0\xe1\x01 \xa0\xe3 \x10\xa0\xe3'
    b'\n\x00\xa0\xe1\xcb\xff\xff\xeb\x0c\xb0\x9d\xe5\x01P\x85\xe2\x01'
    b'\x00[\xe3\xf1\xff\xff\x1a\x1c0\x9d\xe5\x14\x80\x9d\xe5H\x90\x9d'
    b'\xe5\x00\x00S\xe3\x08\x80\x86\xe0\x85\x92\x89\xe0K\x00\x00\x1a'
    b'  \x9d\xe5\x03\x00R\xe1U\x00\x00\x8a\t\x10\xa0\xe1\x00 \xa0\xe3'
    b'\x04\x00\xa0\xe1\xbc\xff\xff\xeb\xb85\xdd\xe1\x05\x00S\xe1\xe5'
    b'\xff\xff\xca\x04\x00\xa0\xe1\xb1\xff\xff\xeb\x00\x00W\xe3/\x00'
    b'\x00\n\x070\x96\xe7\x07p\x86\xe0\x00\x00S\xe3+\x00\x00\n\x00 \xa0'
    b'\xe3\x08\xc0\xa0\xe3\x02\x00\xa0\xe1\x02\xe0\xa0\xe1\x11\x00S\xe3'
    b'\x04\xe0\x97\x05\x04\x00\x00\n\x12\x00S\xe3\x04\x00\x97\x05\x01'
    b'\x00\x00\n\x13\x00S\xe3\x04\xc0\x97\x05\x080\xb7\xe5\x01 \x82\xe2'
    b'\x00\x00S\xe3d\x00R\x13\x01\x10\xa0\x13\x00\x10\xa0\x03\xf0\xff'
    b'\xff\x1a\x00\x00^\xe3\x00\x00P\x13\x15\x00\x00\n\x00\x00\\\xe3'
    b'\x08\xc0\xa0\x03\x0c\x00P\xe1\x11\x00\x00:\x0c\x00@\xe0\x00\x00'
    b'\\\xe1\x01\x10\x81\xe2\xfb\xff\xff\x9a\x00\x00Q\xe3\x0b\x00\x00'
    b'\n\x81\x11\x86\xe0\x0e0\x86\xe0\x0e\x10\x81\xe0\x04 \xd3\xe5'
    b'\x080\x83\xe2\x17\x00R\xe3\x08\x00\x13\x05\x00 \x96\x07\x02 \x86'
    b'\x00\x00 \x86\x07\x01\x00S\xe1\xf6\xff\xff\x1a\x000\xa0\xe3~\xff'
    b'\x17\xee\xfd\xff\xff\x1a\x9a?\x07\xee\x15?\x07\xeeD0\x9d\xe5'
    b'\x03\x00\x86\xe0d\xd0\x8d\xe2\xf0\x8f\xbd\xe8\x04\x00\xa0\xe1t'
    b'\xff\xff\xeb\x00\x00\xa0\xe3d\xd0\x8d\xe2\xf0\x8f\xbd\xe8\x10'
    b'\x10\x9d\xe5\x00 \xa0\xe3\x04\x00\xa0\xe1s\xff\xff\xeb\x1c \x9d'
    b'\xe5\x040\xa0\xe1\x0b\x10\xa0\xe1\x08\x00\xa0\xe1k\xff\xff\xeb'
    b'\x1c0\x9d\xe5  \x9d\xe5\x03\x00R\xe1\xa9\xff\xff\x9a\x030\x88\xe0'
    b'\x02\x80\x88\xe0\x00 \xa0\xe3\x01 \xc3\xe4\x08\x00S\xe1\xfc\xff'
    b'\xff\x1a\xa2\xff\xff\xea\x04\x00\xa0\xe1[\xff\xff\xeb\xda\xff\xff'
    b'\xeah\x02\x00\x00\x7fE\x00\x00r\x00b\x00\x00\x00'
)

ADDR_EVENT_SLOT = 0x307FBFA0
SLOT_TRAP = 0xE51FF004
ST_MAGIC = 0
ST_HOOK_FN = 8
ST_SAVED = 12
ST_SAVED_VALID = 28
ST_ARMED = 32
ST_ALLOW_HOOK = 36

RUN_MAGIC = 0
RUN_STATE = 8
RUN_MODE = 12
RUN_EXIT = 16
RUN_ABORT = 20
RUN_POLICY = 24
RUN_INTERRUPT = 28
RUN_STARTED = 32
RUN_RING = 36
RUN_RING_START = 40
RUN_CFG = 44

RUN_IDLE, RUN_ACTIVE, RUN_DONE = 0, 1, 2

FW_EVENT_STUB = struct.pack("<IIII", 0xE52D0004, 0xE52DE004,
                            0xEF01003F, 0xE49D0004)

class ShellcodeElfLoader:
    def __init__(self, dbg):
        self.dbg = dbg
        self.loader_addr = 0
        self.image_raw = 0
        self.image_base = 0
        self.image_size = 0
        self.entry = 0
        self.ring = 0
        self.run_state = 0
        self.hook_snapshot = None
        self.owned = False
        self.state_addr = 0

    def forget(self):
        self.state_addr = 0
        self.loader_addr = 0
        self.image_raw = 0
        self.image_base = 0
        self.image_size = 0
        self.entry = 0
        self.ring = 0
        self.run_state = 0
        self.owned = False

    def upload_loader(self):
        size = len(LOADER_CODE)
        self.loader_addr = self.dbg.call(ADDR_MALLOC, (size + 3) & ~3)
        if not self.loader_addr:
            return False
        self.dbg.write_mem_bytes(self.loader_addr, LOADER_CODE)
        return True

    def save_hook_snapshot(self):
        if self.hook_snapshot is not None:
            return self.hook_snapshot
        try:
            first = self.dbg.read_mem(ADDR_EVENT_SLOT, 16)
            if not first or len(first) != 16:
                return None
            if struct.unpack_from("<I", first, 0)[0] == SLOT_TRAP:
                launcher_log("[hook] slot holds a stale patch at startup; "
                             "no fresh snapshot")
                return None
            self.hook_snapshot = bytes(first)
            path = self.hook_snap_path()
            if path:
                with uio.FileIO(path, "wb") as h:
                    h.write(self.hook_snapshot)
            launcher_log("[hook] original slot bytes saved: %s"
                         % _hex16(self.hook_snapshot))
            return self.hook_snapshot
        except Exception as e:
            launcher_log("[hook] snapshot failed: %r" % (e,))
            return None

    def restore_stale_hook_slot(self):
        try:
            w0, hook_fn = struct.unpack("<II", bytes(
                self.dbg.read_mem(ADDR_EVENT_SLOT, 8)))
        except Exception:
            return False
        if w0 != SLOT_TRAP:
            return False
        path = self.hook_snap_path()
        data = None
        if path:
            try:
                with uio.FileIO(path, "rb") as h:
                    data = h.read(16)
            except Exception:
                data = None
        if not data or len(data) != 16:
            self.neutralize_hook_slot()
            return False
        if struct.unpack_from("<I", data, 0)[0] == SLOT_TRAP:
            launcher_log("[hook] snapshot is itself a trap; neutralising "
                         "instead of restoring it")
            self.neutralize_hook_slot()
            return False
        try:
            self.write_slot(data)
            launcher_log("[hook] stale patch removed at startup (hook_fn was "
                         "0x%08X)" % hook_fn)
            return True
        except Exception as e:
            launcher_log("[hook] could not clear the stale patch: %r" % (e,))
            return False

    def neutralize_hook_slot(self):
        try:
            w0 = struct.unpack("<I", bytes(self.dbg.read_mem(ADDR_EVENT_SLOT, 4)))[0]
        except Exception:
            return False
        if w0 != SLOT_TRAP:
            return False
        original = self.hook_snapshot or self.snapshot_from_file()
        if original and len(original) == 16 and \
                struct.unpack_from("<I", original, 0)[0] != SLOT_TRAP:
            try:
                self.write_slot(original)
                launcher_log("[hook] stale patch replaced with the saved "
                             "original bytes at startup")
                return True
            except Exception as e:
                launcher_log("[hook] could not write the saved bytes back: %r"
                             % (e,))
        try:
            self.write_slot(FW_EVENT_STUB)
            launcher_log("[hook] stale patch replaced with the firmware's own "
                         "get_event stub (no snapshot was available)")
            return True
        except Exception as e:
            launcher_log("[hook] could not write the stub back: %r" % (e,))
        try:
            nop = struct.pack("<IIII", 0xE1A00000, 0xE1A00000,
                              0xE1A00000, 0xE1A00000)
            self.write_slot(nop)
            launcher_log("[hook] stale patch neutralised at startup (the "
                         "original bytes were not available)")
            return True
        except Exception as e:
            launcher_log("[hook] could not neutralise the stale patch: %r" % (e,))
            return False

    def write_slot(self, data):
        try:
            words = list(struct.unpack("<IIII", bytes(data)[:16]))
        except Exception:
            return False
        try:
            armed_now = struct.unpack(
                "<I", bytes(self.dbg.read_mem(ADDR_EVENT_SLOT, 4)))[0] == SLOT_TRAP
        except Exception:
            armed_now = False
        order = [1, 0, 2, 3] if armed_now else [0, 1, 2, 3]
        for i in order:
            if i == 1 and armed_now:
                try:
                    self.dbg.write_mem(ADDR_EVENT_SLOT + 4, 0)
                except Exception as e:
                    launcher_log("[hook] slot disarm failed: %r" % (e,))
                    return False
            try:
                self.dbg.write_mem(ADDR_EVENT_SLOT + 4 * i, words[i])
            except Exception as e:
                launcher_log("[hook] slot write %d failed: %r" % (i, e))
                return False
        return True

    def hook_snap_path(self):
        return HOOK_SNAP_NAME

    def snapshot_from_file(self):
        try:
            with uio.FileIO(HOOK_SNAP_NAME, "rb") as h:
                d = h.read(16)
            if d and len(d) == 16:
                return d
        except Exception:
            pass
        return None

    def restore_hook_slot(self, why, filename=None):
        if not (self.owned and self.image_base):
            return False
        try:
            first = self.dbg.read_mem(ADDR_EVENT_SLOT, 8)
            if not first or len(first) != 8:
                return False
            word0, hook_fn = struct.unpack("<II", first)
            if word0 != SLOT_TRAP:
                return False
            state = self.find_state_addr()
            if not state:
                return False
            st = self.dbg.read_mem(state, ST_SAVED_VALID + 4)
            if not st or len(st) < ST_SAVED_VALID + 4:
                return False
            if struct.unpack_from("<I", st, ST_MAGIC)[0] != 0x4D495250:
                return False
            if struct.unpack_from("<I", st, ST_HOOK_FN)[0] != hook_fn:
                return False
            original = None
            if struct.unpack_from("<I", st, ST_SAVED_VALID)[0] == 1:
                original = st[ST_SAVED:ST_SAVED + 16]
            if not original or struct.unpack_from("<I", original, 0)[0] == SLOT_TRAP:
                original = self.hook_snapshot or self.snapshot_from_file()
            if not original or struct.unpack_from("<I", original, 0)[0] == SLOT_TRAP:
                return False
            ok = self.write_slot(original)
            back = self.dbg.read_mem(ADDR_EVENT_SLOT, 8)
            w0, w1 = (struct.unpack("<II", bytes(back)) if back and len(back) == 8
                      else (-1, -1))
            launcher_log("[hook] slot restore (%s): wrote %08X %08X, read back "
                         "%08X %08X%s"
                         % (why,
                            struct.unpack_from("<I", original, 0)[0],
                            struct.unpack_from("<I", original, 4)[0],
                            w0, w1, "" if w0 != SLOT_TRAP else
                            "   <-- STILL A TRAP"))
            return ok
        except Exception as e:
            launcher_log("[hook] restore failed (%s): %r" % (why, e))
            return False

    def find_state_addr(self):
        if self.state_addr:
            return self.state_addr
        try:
            with uio.FileIO(LOG_NAME, "rb") as f:
                data = f.read(16384)
        except Exception:
            data = b""
        try:
            txt = data.decode("latin1") if isinstance(data, bytes) else data
        except Exception:
            txt = ""
        addr = 0
        for line in txt.split("\n"):
            line = line.strip()
            if line.startswith("in_state="):
                try:
                    addr = int(line[len("in_state="):].strip(), 16) & 0xFFFFFFFF
                except Exception:
                    addr = 0
        if addr >= 0x30000000:
            self.state_addr = addr
            return addr
        if self.image_base:
            addr = ElfTools.find_magic_addr("./" + ELF_NAME, b"PRIMEIN",
                                            self.image_base)
            if addr:
                self.state_addr = addr
        return self.state_addr

    def stand_down_hook(self):
        ok = False
        try:
            if not self.image_base:
                launcher_log("[hook] stand-down: no image record")
                return False
            state = self.find_state_addr()
            if not state:
                launcher_log("[hook] stand-down: no input state address "
                             "(plua.log has no in_state= line and the "
                             "lua.elf scan found nothing)")
                return False
            head = self.dbg.read_mem(state, 4)
            if not head or len(head) != 4 or \
                    struct.unpack("<I", head)[0] != 0x4D495250:
                launcher_log("[hook] stand-down: PRIMEIN magic missing at "
                             "0x%08X" % state)
                return False
            self.dbg.write_mem(state + ST_ARMED, 0)
            back = self.dbg.read_mem(state + ST_ARMED, 4)
            armed = struct.unpack("<I", bytes(back))[0] if back and len(back) == 4 else -1
            ok = (armed == 0)
            launcher_log("[hook] stand-down: armed=%d (state 0x%08X)%s"
                         % (armed, state, "" if ok else "  <-- WRITE FAILED"))
            try:
                sl = self.dbg.read_mem(ADDR_EVENT_SLOT, 8)
                if sl and len(sl) == 8:
                    w0, w1 = struct.unpack("<II", bytes(sl))
                    launcher_log("[hook] slot now: %08X %08X (%s)"
                                 % (w0, w1,
                                    "firmware stub" if w0 != SLOT_TRAP
                                    else "STILL OUR TRAP"))
            except Exception:
                pass
        except Exception as e:
            launcher_log("[hook] stand-down failed: %r" % (e,))
            return False
        return ok

    def flag(self, name, app_dir="", files=None):
        for p in (((app_dir + "\\" + name) if app_dir else ""), name):
            if not p:
                continue
            try:
                if files is not None and files.exists(p):
                    return True
            except Exception:
                pass
        try:
            import os
            return bool(os.environ.get("PLUA_" + name.upper()))
        except Exception:
            return False

    def apply_touch_hook_setting(self, app_dir, files=None):
        return False

    def load_elf(self, filename, full_path):
        need = ElfTools.get_elf_memory_size(filename)
        if not need:
            launcher_log("[load] FAIL: %s has no readable ELF header "
                         "(need=%d)" % (filename, need))
            return 0, 0
        launcher_log("[load] %s needs %d B" % (filename, need))
        if not (self.owned and self.image_raw and self.image_size >= need):
            if self.owned and self.image_raw:
                launcher_log("[load] freeing our previous image 0x%08X"
                             % self.image_raw)
                self.restore_hook_slot("reload", filename)
                self.dbg.call(ADDR_FREE, self.image_raw)
                self.image_raw = 0
            raw = self.dbg.call(ADDR_MALLOC, need + 16)
            if not raw:
                launcher_log("[load] FAIL: firmware malloc(%d) returned 0"
                             % (need + 16,))
                return 0, 0
            self.image_raw = raw
            self.image_base = (raw + 15) & ~7
            self.image_size = need
            self.owned = True
            launcher_log("[load] image block 0x%08X (%d B), base 0x%08X"
                         % (raw, need, self.image_base))
        path_bytes = str_to_utf16le_bytes(full_path)
        path_addr = self.dbg.call(ADDR_MALLOC, len(path_bytes))
        if not path_addr:
            launcher_log("[load] FAIL: malloc(%d) for the path returned 0"
                         % len(path_bytes))
            return 0, 0
        self.dbg.write_mem_bytes(path_addr, path_bytes)
        try:
            self.entry = self.dbg.call(self.loader_addr, path_addr,
                                       self.image_base)
        finally:
            self.dbg.call(ADDR_FREE, path_addr)
        if not self.entry or self.entry < 0x30000000:
            launcher_log("[load] FAIL: the shellcode loader returned entry "
                         "0x%08X (image_base 0x%08X, need %d B)"
                         % (self.entry, self.image_base, need))
            return 0, 0
        ring = ElfTools.find_magic_addr(filename, b"PLUARING",
                                        self.image_base)
        self.ring = ring
        self.run_state = ElfTools.find_magic_addr(filename, b"PLUARUN",
                                                  self.image_base)
        if not ring:
            launcher_log("[load] FAIL: no PLUARING ring in %s (image_base "
                         "0x%08X)" % (filename, self.image_base))
        return self.entry, ring

    def unload(self):
        if self.owned and self.image_raw:
            if not self.hand_back_input_before_free("unload"):
                return
            self.dbg.call(ADDR_FREE, self.image_raw)
        self.image_raw = 0
        self.image_base = 0
        self.image_size = 0
        self.owned = False
        if self.loader_addr:
            self.dbg.call(ADDR_FREE, self.loader_addr)
            self.loader_addr = 0
        self.entry = 0
        self.ring = 0
        self.state_addr = 0

    def hand_back_input_before_free(self, why):
        try:
            w0 = struct.unpack("<I",
                               bytes(self.dbg.read_mem(ADDR_EVENT_SLOT, 4)))[0]
        except Exception:
            w0 = SLOT_TRAP
        if w0 != SLOT_TRAP:
            return True
        if self.restore_hook_slot(why):
            launcher_log("[hook] firmware input handed back before freeing "
                         "the image (%s)" % why)
            return True
        launcher_log("[hook] REFUSING to free the image: the firmware slot is "
                     "still our trap and it could not be restored (%s) -- "
                     "leaking the block instead of resetting the calculator"
                     % why)
        return False

CFG_MAGIC = 0x41554C50
LOG_NAME = "plua.log"

class ConfigBuilder:
    def __init__(self, dbg, mem):
        self.dbg = dbg
        self.mem = mem
        self.allocations = []
        self.cfg_addr = 0

    def _str(self, s):
        b = s.encode("utf-8") + b"\x00"
        addr = self.mem.malloc(len(b))
        self.dbg.write_mem_bytes(addr, b)
        self.allocations.append(addr)
        return addr

    def build(self, argv, epoch=-1, heap_free=0, logpath=None, homedir=None):
        log_addr = self._str(logpath) if logpath else 0
        home_addr = self._str(homedir) if homedir else 0
        ptrs = [self._str(a) for a in argv]
        arr_addr = self.mem.malloc(len(ptrs) * 4)
        self.dbg.write_mem_bytes(arr_addr, b"".join(
            struct.pack("<I", p) for p in ptrs))
        self.allocations.append(arr_addr)
        cfg = struct.pack("<IIIIIII", CFG_MAGIC, len(argv), arr_addr,
                          epoch & 0xFFFFFFFF, heap_free, log_addr, home_addr)
        cfg_addr = self.mem.malloc(len(cfg))
        self.dbg.write_mem_bytes(cfg_addr, cfg)
        self.allocations.append(cfg_addr)
        self.cfg_addr = cfg_addr
        return cfg_addr

    def free_all(self):
        for a in reversed(self.allocations):
            self.mem.free(a)
        self.allocations = []

class RingReader:

    def __init__(self, dbg, addr):
        self.dbg = dbg
        self.addr = addr

    def count(self):
        if not self.addr:
            return 0
        try:
            return struct.unpack("<I", self.dbg.read_mem(self.addr + 8, 4))[0]
        except Exception:
            return 0

    def _chunk(self, start, n):
        if n <= 0:
            return b""
        take = min(n, RING_SIZE - start)
        try:
            data = self.dbg.read_mem(self.addr + 12 + start, take)
            if n > take:
                data += self.dbg.read_mem(self.addr + 12, n - take)
        except Exception:
            return b""
        return data

    def read_since(self, since):
        total = self.count()
        if total <= since:
            return b""
        n = min(total - since, RING_SIZE)
        start = since % RING_SIZE
        return self._chunk(start, n)

    def text(self, since=0):
        txt = decode_bytes(self.read_since(since))
        code = None
        keep = []
        for line in txt.split("\n"):
            if line.startswith("RT_RET:"):
                try:
                    code = int(line[7:].strip())
                except Exception:
                    code = None
                continue
            keep.append(line)
        return "\n".join(keep), code

    def read_from(self, pos):
        total = self.count()
        if total <= pos:
            return b"", pos, 0
        lost = total - pos - RING_SIZE
        if lost > 0:
            pos += lost
        n = total - pos
        data = self._chunk(pos % RING_SIZE, n)
        return data, total, max(lost, 0)

STREAM_POLL_MS = 50
STREAM_POLL_IDLE_MS = 150
STREAM_IDLE_AFTER = 3
STREAM_KEY_EVERY = 1
STREAM_READ_FAILS = 50
K_UP, K_DOWN, K_LEFT, K_RIGHT = 2, 12, 7, 8
K_ENTER, K_ESC, K_BACKSPACE = 30, 4, 19
K_SYMB, K_HELP, K_VIEW = 1, 3, 9

STREAM_ABORT_KEYS = (K_ESC,)

STREAM_ON_LABEL = "ON"

def _ticks():
    try:
        return int(hpprime.ticks()) if hpprime else 0
    except Exception:
        return 0

def _stream_pause(dbg, ms=STREAM_POLL_MS):
    try:
        dbg.call(ADDR_SLEEP, ms)
        return
    except Exception:
        pass
    _pace(ms)

def _abort_key_down(hp):
    try:
        mask = int(hp.keyboard())
    except Exception:
        return False
    for k in STREAM_ABORT_KEYS:
        if mask & (1 << k):
            return True
    return False

class RunState:

    def __init__(self, dbg, addr):
        self.dbg = dbg
        self.addr = addr or 0
        self.valid = False
        if self.addr:
            try:
                magic = bytes(dbg.read_mem(self.addr + RUN_MAGIC, 8))
                self.valid = magic == b"PLUARUN\x00"
            except Exception:
                self.valid = False

    def word(self, off, default=-1):
        try:
            raw = self.dbg.read_mem(self.addr + off, 4)
            if raw and len(raw) == 4:
                return struct.unpack("<I", bytes(raw))[0]
        except Exception:
            pass
        return default

    def state(self):
        return self.word(RUN_STATE)

    def mode(self):
        return self.word(RUN_MODE)

    def exit_code(self):
        v = self.word(RUN_EXIT)
        if v is None or v == -1 or v > 0x7FFFFFFF:
            return None
        return v

    def ring(self):
        return self.word(RUN_RING, 0)

    def ring_start(self):
        return self.word(RUN_RING_START, -1)

    def request_abort(self):
        try:
            self.dbg.write_mem(self.addr + RUN_ABORT, 1)
            return True
        except Exception:
            return False

    def configure(self, thread=True, interrupt=True):
        if not self.valid:
            return False
        try:
            self.dbg.write_mem(self.addr + RUN_POLICY, 1 if thread else 0)
            self.dbg.write_mem(self.addr + RUN_INTERRUPT,
                               1 if interrupt else 0)
            return True
        except Exception:
            return False

class FileList:
    def __init__(self, dbg, mem):
        self.dbg = dbg
        self.mem = mem

    def attr(self, path):
        p = self.mem.alloc_wstr(path)
        try:
            return self.dbg.call(W_GET_ATTR, p)
        finally:
            self.mem.free(p)

    def exists(self, path):
        return self.attr(path) != 0xFFFFFFFF

    def cwd(self):
        try:
            disk = self.dbg.call(GET_DISK_ID)
            buf = self.mem.malloc(520)
            try:
                if self.dbg.call(W_GETCWD, disk, buf) != 0xFFFFFFFF:
                    return self.mem.read_wstr(buf)
            finally:
                self.mem.free(buf)
        except Exception:
            pass
        return ""

    def _find(self, pattern, mask):
        p = self.mem.alloc_wstr(pattern)
        ctx = self.mem.malloc(256)
        out = []
        try:
            if mask is None:
                ok = self.dbg.call(W_FIND_FIRST, p, ctx)
            else:
                ok = self.dbg.call(W_FIND_FIRST, p, ctx, mask)
            if ok != 0:
                return []
            while True:
                raw = self.dbg.read_mem(ctx, 40)
                lfn = struct.unpack_from("<I", raw, 8)[0]
                sfn = struct.unpack_from("<I", raw, 12)[0]
                size = struct.unpack_from("<I", raw, 20)[0]
                attr = struct.unpack_from("<B", raw, 37)[0]
                name = self.mem.read_wstr(lfn) or self.mem.read_utf8(sfn)
                if name and name not in (".", ".."):
                    out.append({"name": name, "size": size, "attr": attr,
                                "is_dir": (attr & 0x10) != 0})
                if self.dbg.call(W_FIND_NEXT, ctx) != 0:
                    break
        except Exception:
            pass
        finally:
            try:
                self.dbg.call(FIND_CLOSE, ctx)
            except Exception:
                pass
            self.mem.free(p)
            self.mem.free(ctx)
        return out

    def list_dir(self, directory):
        pattern = directory.rstrip("\\") + "\\*.lua"
        seen = {}
        for mask in (0x37, 0xFF, 0x00, None):
            for it in self._find(pattern, mask):
                if not it["is_dir"] and it["name"].lower().endswith(".lua"):
                    seen[it["name"]] = it
        out = []
        for name in sorted(seen, key=lambda n: n.lower()):
            it = seen[name]
            it["dir"] = directory
            it["path"] = directory.rstrip("\\") + "\\" + name
            out.append(it)
        return out

ELF_NAME = "lua.elf"
HOOK_SNAP_NAME = "hook.bin"
LOG_NAME = "plua.log"
LAUNCHER_LOG = "plua_launcher.log"

SLOT_NAME = "lua.slot"
SLOT_MAGIC = 0x504C5541
SLOT_FMT = "<11I"
SLOT_LEN = 44

DEFAULT_SCRIPT = "hello.lua"

APP_DIRS = ["C:\\DATA\\primeLua.hpappdir", "C:\\DATA", "\\"]

class PrimeLua:

    def __init__(self, hp=None):
        self.hp = hp if hp is not None else hpprime
        self.dbg = PrimeDebug()
        self.mem = MemUtils(self.dbg)
        self.files = FileList(self.dbg, self.mem)
        self.loader = ShellcodeElfLoader(self.dbg)
        self.app_dir = None
        self.hook_snapshot = None
        self.scripts = []
        self.epoch = -1
        self.error = ""
        self.out_lines = []
        self.out_code = None
        self.streamed = False
        self.run_alive = False

    def setup(self):
        self.app_dir = self.find_app_dir()
        self.loader.save_hook_snapshot()
        self.set_hook_policy()
        self.loader.neutralize_hook_slot()
        self.loader.restore_stale_hook_slot()
        self.epoch = self.read_epoch()
        self.rescan()
        return self

    def set_hook_policy(self):
        try:
            allow = 0 if self.loader.flag("noinputhook", self.app_dir,
                                          self.files) else 1
        except Exception:
            allow = 1
        addr = self.loader.find_state_addr()
        if not addr:
            return None
        try:
            self.dbg.write_mem(addr + ST_ALLOW_HOOK, allow)
            back = self.dbg.read_mem(addr + ST_ALLOW_HOOK, 4)
            got = struct.unpack("<I", bytes(back))[0] if back and len(back) == 4 else -1
        except Exception as e:
            launcher_log("[hook] policy write failed: %r" % (e,))
            return None
        launcher_log("[hook] input interception %s (marker file "
                     "'noinputhook' disables it)"
                     % ("ALLOWED" if got == 1 else "OFF"))
        return got

    def find_app_dir(self):
        cands = []
        cwd = self.files.cwd()
        if cwd:
            cands.append(cwd.rstrip("\\"))
        cands.extend(APP_DIRS)
        for d in cands:
            try:
                if self.files.exists(d + "\\" + ELF_NAME):
                    return d
            except Exception:
                continue
        return APP_DIRS[0]

    def rescan(self):
        self.scripts = []
        dirs = [self.app_dir]
        cwd = self.files.cwd()
        if cwd and cwd.rstrip("\\") not in [d.rstrip("\\") for d in dirs]:
            dirs.append(cwd.rstrip("\\"))
        if "C:\\DATA" not in dirs:
            dirs.append("C:\\DATA")
        for d in dirs:
            try:
                self.scripts.extend(self.files.list_dir(d))
            except Exception:
                pass
        return self.scripts

    def heap_free(self):
        if self.hp is None:
            return 0
        try:
            r = self.hp.eval("memory(1)")
            try:
                return int(r)
            except Exception:
                return int(r[0])
        except Exception:
            return 0

    @staticmethod
    def split_ppl_date(v):
        try:
            return int(v[0]), int(v[1]), int(v[2])
        except Exception:
            pass
        y = int(v)
        md = int(round((v - y) * 10000 + 0.0000001))
        return y, md // 100, md % 100

    @staticmethod
    def split_ppl_time(v):
        try:
            return int(v[0]), int(v[1]), int(v[2])
        except Exception:
            pass
        hh = int(v)
        minutes = (v - hh) * 60.0
        mi = int(minutes)
        ss = int(round((minutes - mi) * 60.0))
        if ss >= 60:
            ss -= 60
            mi += 1
        if mi >= 60:
            mi -= 60
            hh += 1
        return hh, mi, ss

    def read_epoch(self):
        if self.hp is None:
            return -1
        try:
            d = self.hp.eval("Date")
            t = self.hp.eval("Time")
            y, mo, da = self.split_ppl_date(d)
            hh, mi, ss = self.split_ppl_time(t)
        except Exception:
            return -1
        if not (1970 <= y <= 9999 and 1 <= mo <= 12 and 1 <= da <= 31
                and hh <= 23 and mi <= 59 and ss <= 60):
            return -1
        y -= mo <= 2
        era = (y if y >= 0 else y - 399) // 400
        yoe = y - era * 400
        doy = (153 * (mo + (-3 if mo > 2 else 9)) + 2) // 5 + da - 1
        doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
        days = era * 146097 + doe - 719468
        return days * 86400 + hh * 3600 + mi * 60 + ss

    def clear_log(self):
        try:
            with uio.FileIO(LOG_NAME, "wb") as f:
                f.write(b"")
        except Exception:
            pass

    def read_log(self):
        try:
            with uio.FileIO(LOG_NAME, "rb") as f:
                data = f.read(8192)
        except Exception:
            return []
        return [ln for ln in decode_bytes(data).replace("\r", "").split("\n")
                if ln]

    def log_tail(self):
        lines = self.read_log()
        for i in range(len(lines) - 1, -1, -1):
            if lines[i].startswith("---- run ----"):
                return lines[i:]
        return lines

    def run_script(self, path, name=None, quiet=False, args=()):
        name = name or path.rsplit("\\", 1)[-1]
        self.error = ""
        self.out_lines = []
        self.out_code = None
        streamed = False
        self.run_alive = False
        elf_path = self.app_dir + "\\" + ELF_NAME

        if not quiet:
            p_log("[*] running {} ...".format(name))
        launcher_log("[run] " + name)
        try:
            if not self.loader.entry:
                self.adopt_slot()
            if not self.loader.loader_addr:
                if not self.loader.upload_loader():
                    self.error = "loader upload failed (firmware malloc)"
                    p_error("[-] " + self.error)
                    return None
            if self.loader.entry and self.loader.ring:
                entry, ring = self.loader.entry, self.loader.ring
            else:
                entry, ring = self.loader.load_elf(ELF_NAME, elf_path)
                if entry and ring:
                    self.write_slot()
        except Exception as e:
            self.error = "load failed: %r" % (e,)
            p_error("[-] " + self.error)
            return None
        if entry and ring:
            self.loader.apply_touch_hook_setting(self.app_dir, self.files)
        if not entry or not ring:
            self.error = "cannot load {}".format(elf_path)
            p_error("[-] " + self.error)
            return None

        cfg = ConfigBuilder(self.dbg, self.mem)
        try:
            self.clear_log()
            self.epoch = self.read_epoch()
            cfg_addr = cfg.build(["lua", path] + list(args),
                                 epoch=self.epoch,
                                 heap_free=self.heap_free(),
                                 logpath=self.app_dir + "\\" + LOG_NAME,
                                 homedir=self.app_dir)
            reader = RingReader(self.dbg, ring)
            mark = reader.count()
            run = RunState(self.dbg, self.loader.run_state or 0)
            if run.valid:
                try:
                    threaded = not self.loader.flag("nothread", self.app_dir,
                                                    self.files)
                except Exception:
                    threaded = True
                # The abort hook more than doubles the VM's work (measured:
                # port/plua_run.c), so it is armed only when the user asks for
                # it -- an empty file named "interrupt" in the app folder means
                # "I want ON/ESC to stop even a pure numeric loop, whatever it
                # costs".  Without it a printing/drawing/sleeping script is
                # still interruptible; a numeric one runs to the end.
                try:
                    want_hook = bool(self.loader.flag("interrupt", self.app_dir,
                                                      self.files))
                except Exception:
                    want_hook = False
                run.configure(thread=threaded, interrupt=want_hook)
                launcher_log("[stream] abort hook %s (marker file 'interrupt')"
                             % ("ARMED" if want_hook else "off"))
                launcher_log("[stream] mode=%s run=0x%08X ring=0x%08X"
                             % ("thread" if threaded else "inline",
                                run.addr, run.ring() or 0))
            else:
                launcher_log("[stream] no PLUARUN state in the image: "
                             "running synchronously, output after exit")
            launcher_log("[stream] calling entry 0x%08X" % entry)
            try:
                ret = self.dbg.call(entry, cfg_addr, 0)
            except KeyboardInterrupt:
                launcher_log("[stream] ON during the entry call")
                p_warning("[!] interrupting %s (ON)" % name)
                ret = 0
            except Exception as e:
                self.error = "call failed: %r" % (e,)
                p_error("[-] " + self.error)
                ret = 0
            launcher_log("[stream] entry returned 0x%08X" % (ret or 0))
            handed = RunState(self.dbg, ret)
            if handed.valid:
                if not run.valid or handed.addr != run.addr:
                    launcher_log("[stream] run state from the entry: 0x%08X"
                                 % handed.addr)
                self.loader.run_state = handed.addr
                run = handed
            if run.valid and run.state() == RUN_ACTIVE:
                streamed = True
                self.stream_run(run, reader, name, quiet)
            txt, code = reader.text(mark)
        finally:
            if self.run_alive:
                launcher_log("[stream] config NOT freed (run still active)")
            else:
                cfg.free_all()
            if self.loader.stand_down_hook():
                launcher_log("[hook] disarmed after " + name)
            else:
                launcher_log("[hook] NOT disarmed after " + name)
            if self.loader.restore_hook_slot("script end"):
                launcher_log("[hook] slot handed back after " + name)
            self.set_hook_policy()

        self.out_lines = [ln for ln in txt.replace("\r", "").split("\n")]
        while self.out_lines and self.out_lines[-1] == "":
            self.out_lines.pop()
        self.out_code = code

        launcher_log("[exit] {} -> {}".format(name, code))
        try:
            free = self.heap_free()
            if free:
                launcher_log("[mem] after {}: heap_free={} ({:.0f} KB)"
                             .format(name, free, free / 1024.0))
        except Exception:
            pass
        if not quiet:
            if not streamed:
                self.emit(self.out_lines)
            for line in self.log_tail():
                low = line.lower()
                bad = "fail" in low and "fail=0" not in low                     and "failed=0" not in low
                if bad:
                    p_warning("[!] " + line)
        return code

    def stream_run(self, run, reader, name, quiet=False):
        pos = run.ring_start()
        if pos is None or pos < 0 or pos > reader.count():
            pos = reader.count()
        pending = ""
        polls = 0
        fails = 0
        idle = 0
        asked = None
        given_up = False
        t0 = _ticks()

        def interrupt(why):
            nonlocal asked
            if asked:
                return
            asked = why
            run.request_abort()
            launcher_log("[stream] interrupt (%s) -> %s" % (why, name))
            if not quiet:
                p_warning("[!] interrupting %s (%s)" % (name, why))

        st = RUN_ACTIVE
        while True:
            try:
                data, pos, lost = reader.read_from(pos)
                if lost:
                    pending = ""
                if data:
                    pending = self.emit_stream(pending, data, quiet)
                    idle = 0
                else:
                    idle += 1
                if (polls % 2) == 0 or data:
                    st = run.state()
                    if st is None:
                        fails += 1
                        if fails == STREAM_READ_FAILS and not given_up:
                            given_up = True
                            self.error = "the run state became unreadable"
                            p_error("[-] " + self.error)
                    else:
                        fails = 0
                        if st != RUN_ACTIVE:
                            break
                polls += 1
                if (not quiet and polls % STREAM_KEY_EVERY == 0
                        and _abort_key_down(self.hp)):
                    interrupt("ESC")
            except KeyboardInterrupt:
                interrupt(STREAM_ON_LABEL)
                _stream_pause(self.dbg, STREAM_POLL_MS // 5)
                continue
            try:
                if not data and idle > STREAM_IDLE_AFTER:
                    _stream_pause(self.dbg, STREAM_POLL_IDLE_MS)
                else:
                    _stream_pause(self.dbg, STREAM_POLL_MS)
            except KeyboardInterrupt:
                interrupt(STREAM_ON_LABEL)
                _stream_pause(self.dbg, STREAM_POLL_MS // 5)

        dt = _ticks() - t0
        per = (dt / polls) if polls else 0
        launcher_log("[stream] %s: %d poll(s) in %d ms (%.1f ms/poll), %s"
                     % (name, polls, dt, per,
                        ("interrupted by " + asked) if asked else "finished"))
        if polls > 20 and per < STREAM_POLL_MS / 2:
            launcher_log("[stream] WARNING: polls are not sleeping (%.1f ms "
                         "asked for %d) -- the launcher is competing with the "
                         "program" % (per, STREAM_POLL_MS))
        data, pos, lost = reader.read_from(pos)
        if lost:
            pending = ""
        if data:
            pending = self.emit_stream(pending, data, quiet)
        if pending.strip():
            if not pending.startswith("RT_RET:"):
                line = pending.replace("\r", "")
                self.out_lines.append(line)
                if not quiet:
                    self.emit([line])
        self.streamed = True
        if run.state() == RUN_ACTIVE:
            self.run_alive = True
            launcher_log("[stream] the run was still active when streaming "
                         "stopped: refusing to free its config")
        if asked and not quiet:
            p_warning("[!] interrupted (%s)" % asked)

    def emit_stream(self, pending, data, quiet=False):
        text = pending + decode_bytes(data)
        idx = text.rfind("\n")
        if idx < 0:
            return text
        head = text[:idx]
        rest = text[idx + 1:]
        for ln in head.split("\n"):
            ln = ln.replace("\r", "")
            if ln.startswith("RT_RET:"):
                continue
            self.out_lines.append(ln)
            if not quiet:
                self.emit([ln])
        return rest

    @staticmethod
    def emit(lines):
        for line in lines:
            if not line:
                continue
            low = line.lower()
            if (low.startswith("lua:") or "stack traceback" in low
                    or "attempt to " in low or ": error" in low):
                p_error(line)
            else:
                p_out(line)

    def _word(self, addr):
        try:
            b = self.dbg.read_mem(addr, 4)
            if b and len(b) == 4:
                return struct.unpack("<I", b)[0]
        except Exception:
            pass
        return None

    def fingerprints(self):
        return (self._word(self.loader.image_base),
                self._word(self.loader.entry))

    def adopt_slot(self):
        try:
            with uio.FileIO(SLOT_NAME, "rb") as f:
                data = f.read(SLOT_LEN)
        except Exception:
            return False
        if len(data) < SLOT_LEN:
            return False
        vals = struct.unpack(SLOT_FMT, data)
        magic, loader, raw, base, size, entry, ring, fp0, fp1, esize, esum = vals
        if magic != SLOT_MAGIC or not entry or not ring or not base:
            return False
        if entry < 0x30000000 or ring < 0x30000000:
            return False
        try:
            if self.dbg.read_mem(ring, 8) != b"PLUARING":
                return False
        except Exception:
            return False

        self.loader.loader_addr = loader
        self.loader.image_raw = raw
        self.loader.image_base = base
        self.loader.image_size = size
        self.loader.entry = entry
        self.loader.ring = ring
        now0, now1 = self.fingerprints()

        if (now0, now1) != (fp0, fp1) or now0 is None:
            self.loader.forget()
            launcher_log("[slot] rejected: mem=(%s,%s) record=(%s,%s) -- "
                         "loading a fresh copy, nothing freed"
                         % (now0, now1, fp0, fp1))
            p_warning("[!] lua.slot does not match this memory; loading again")
            return False

        hdr = self.dbg.read_mem(base, 16)
        if not hdr or len(hdr) < 16 or hdr[0:4] != b"\x7fELF":
            self.loader.forget()
            launcher_log("[slot] rejected: no ELF header at 0x%08X -- "
                         "loading a fresh copy, nothing freed" % base)
            p_warning("[!] the reused interpreter is gone; loading again")
            return False
        if self.dbg.read_mem(ring, 8) != b"PLUARING":
            self.loader.forget()
            launcher_log("[slot] rejected: no PLUARING ring at 0x%08X -- "
                         "loading a fresh copy, nothing freed" % ring)
            p_warning("[!] the reused interpreter is gone; loading again")
            return False

        on_disk = ElfTools.file_fingerprint(ELF_NAME)
        if (on_disk is not None and on_disk != (esize, esum)) or \
                (on_disk is None and (esize, esum) != (0, 0)):
            self.loader.loader_addr = loader
            self.loader.image_raw = raw
            self.loader.image_base = base
            self.loader.image_size = size
            self.loader.entry = entry
            self.loader.ring = ring
            self.loader.owned = True
            if self.loader.hand_back_input_before_free("lua.elf replaced"):
                try:
                    self.dbg.call(ADDR_FREE, raw)
                    if loader:
                        self.dbg.call(ADDR_FREE, loader)
                except Exception:
                    pass
            self.loader.forget()
            self.drop_slot()
            launcher_log("[slot] lua.elf on disk is now %s, the resident image "
                         "is %s -- old image released, loading the new one"
                         % (on_disk, (esize, esum) if (esize, esum) != (0, 0)
                            else "unknown"))
            p_warning("[!] lua.elf was replaced; loading the new interpreter")
            return False

        p_log("[*] reusing the interpreter already in memory "
              "(base 0x{:08X})".format(base))
        launcher_log("[slot] reused image at 0x%08X (entry 0x%08X)"
                     % (base, entry))
        return True

    def write_slot(self):
        fp0, fp1 = self.fingerprints()
        if fp0 is None or fp1 is None:
            return
        disk = ElfTools.file_fingerprint(ELF_NAME) or (0, 0)
        try:
            with uio.FileIO(SLOT_NAME, "wb") as f:
                f.write(struct.pack(SLOT_FMT, SLOT_MAGIC,
                                    self.loader.loader_addr,
                                    self.loader.image_raw,
                                    self.loader.image_base,
                                    self.loader.image_size,
                                    self.loader.entry, self.loader.ring,
                                    fp0, fp1, disk[0], disk[1]))
        except Exception:
            pass

    def drop_slot(self):
        try:
            with uio.FileIO(SLOT_NAME, "wb") as f:
                f.write(b"\x00" * SLOT_LEN)
        except Exception:
            pass

    def unload(self):
        self.loader.unload()
        self.drop_slot()

_SESSION = None

def session():
    global _SESSION
    if _SESSION is None:
        _SESSION = PrimeLua().setup()
    return _SESSION

def list_files():
    s = session()
    p_log("[*] app folder: {}".format(s.app_dir))
    if not s.scripts:
        p_warning("[!] no .lua files found")
    for it in s.scripts:
        p_log("    {:<24} {:>7} B  {}".format(it["name"], it["size"],
                                               it["dir"]))
    return s.scripts

def run_file(name, quiet=False, args=()):
    s = session()
    path = name if "\\" in name else (s.app_dir + "\\" + name)
    code = s.run_script(path, name.rsplit("\\", 1)[-1], quiet=quiet, args=args)
    if code is not None:
        if code == 0:
            p_pass("[+] exit 0, {} line(s) of output".format(len(s.out_lines)))
        else:
            p_error("[-] exit {}".format(code))
    return s.out_lines

def run_source(text, name="=console", quiet=False):
    s = session()
    rel = "plua_console.lua"
    try:
        with uio.FileIO(rel, "wb") as f:
            f.write(text.encode("utf-8"))
    except Exception as e:
        p_error("[-] cannot write {}: {!r}".format(rel, e))
        return []
    return run_file(s.app_dir + "\\" + rel, quiet=quiet)

def run(script=None):
    return run_file(script or DEFAULT_SCRIPT)

def unload():
    global _SESSION
    if _SESSION is not None:
        _SESSION.unload()
        _SESSION = None

def _release_at_exit():
    global _SESSION
    try:
        if _SESSION is not None:
            _SESSION.unload()
            _SESSION = None
    except Exception as e:
        try:
            launcher_log("[exit] unload failed: %r" % (e,))
        except Exception:
            pass

def selftest(script=None):
    out = []

    def step(name, fn):
        try:
            res = fn()
            msg = "ok   {:<26} {}".format(name, res if res is not None else "")
            out.append(msg)
            launcher_log("[selftest] " + msg)
            p_pass("[+] " + msg)
            return res
        except Exception as e:
            msg = "FAIL {:<26} {!r}".format(name, e)
            out.append(msg)
            launcher_log("[selftest] " + msg)
            p_error("[-] " + msg)
            return None

    if hpprime is None:
        p_warning("[!] not running on the calculator")
        return ["FAIL hpprime missing"]
    s = _SESSION if _SESSION is not None else PrimeLua()

    def check_debug():
        blk = s.mem.malloc(8)
        if not blk:
            raise RuntimeError("firmware malloc failed")
        try:
            s.dbg.write_mem(blk, 0xA5A5A5A5)
            s.dbg.write_mem(blk + 4, 0x5A5A5A5A)
            got = s.dbg.read_mem(blk, 8)
            want = struct.pack("<II", 0xA5A5A5A5, 0x5A5A5A5A)
            if got != want:
                raise RuntimeError("read back %r" % (got,))
            return "write/read back at 0x%08X" % blk
        finally:
            s.mem.free(blk)
    step("debug interface", check_debug)
    step("firmware heap", lambda: "%d KB free" % (s.heap_free() // 1024))
    step("app dir", lambda: s.find_app_dir())
    step("list scripts", lambda: "%d found" % len(s.rescan()))

    def check_elf():
        need = ElfTools.get_elf_memory_size(ELF_NAME)
        if not need:
            raise RuntimeError("cannot read %s through uio" % ELF_NAME)
        return "%d bytes image" % need
    step("read lua.elf header", check_elf)

    def check_loader():
        if s.loader.loader_addr:
            return "shellcode already at 0x%08X" % s.loader.loader_addr
        if not s.loader.upload_loader():
            raise RuntimeError("firmware malloc failed")
        return "shellcode at 0x%08X" % s.loader.loader_addr
    step("upload shellcode loader", check_loader)

    def load():
        if not (s.loader.entry and s.loader.ring):
            entry, ring = s.loader.load_elf(ELF_NAME,
                                            s.app_dir + "\\" + ELF_NAME)
            if entry and ring:
                s.write_slot()
        if not s.loader.entry:
            raise RuntimeError("loader returned no entry point")
        if not s.loader.ring:
            raise RuntimeError("PLUARING ring not found in the image")
        return "entry 0x%08X, ring 0x%08X" % (s.loader.entry, s.loader.ring)
    loaded = step("load lua.elf", load)

    if loaded and script:
        def go():
            cfg = ConfigBuilder(s.dbg, s.mem)
            try:
                addr = cfg.build(["lua", script], epoch=s.epoch,
                                 heap_free=s.heap_free(),
                                 logpath=s.app_dir + "\\" + LOG_NAME,
                                 homedir=s.app_dir)
                entry, ring = s.loader.entry, s.loader.ring
                reader = RingReader(s.dbg, ring)
                mark = reader.count()
                s.dbg.call(entry, addr, 0)
                txt, code = reader.text(mark)
                PrimeLua.emit([ln for ln in txt.split("\n") if ln])
                return "exit %s, %d B of output" % (code, len(txt))
            finally:
                cfg.free_all()
        step("run " + script, go)

    for line in s.read_log()[-6:]:
        p_log("    " + line)
    return out

PROBE = r"""
local function probe(name, fn)
  local ok, err = pcall(fn)
  if ok then
    print(string.format("%-22s ok", name))
  else
    print(string.format("%-22s FAIL %s", name, tostring(err)))
  end
end

probe("print", function() print("   (probe output)") end)
probe("integer arithmetic", function() assert(7 // 2 == 3) assert(2^31 == 2147483648) end)
probe("float arithmetic", function() assert(1/3 > 0.333 and 1/3 < 0.334) end)
probe("string.format %.14g", function() assert(string.format("%.14g", 1/3) == "0.33333333333333") end)
probe("string.format %d/%x", function() assert(string.format("%d %x", 255, 255) == "255 ff") end)
probe("string.format %f/%e", function() assert(string.format("%.3f", 1.5) == "1.500") end)
probe("string library", function() assert(("abc"):upper() == "ABC") assert(#string.rep("x", 10) == 10) end)
probe("patterns (gsub/find)", function() assert(("a1"):gsub("%d", "2") == "a2") end)
probe("tables", function() local t = {1,2,3} assert(#t == 3) assert(table.concat(t, ",") == "1,2,3") end)
probe("table growth", function() local t = {} for i = 1, 500 do t[i] = i end assert(#t == 500) end)
probe("table.sort", function() local t = {3,1,2} table.sort(t) assert(t[1] == 1) end)
probe("ipairs/pairs", function() local t = {1,2} local n = 0 for _ in ipairs(t) do n = n + 1 end assert(n == 2) end)
probe("closures/upvalues", function() local n = 0 local f = function() n = n + 1 return n end assert(f() == 1 and f() == 2) end)
probe("metatables", function() local m = setmetatable({}, {__index = function() return 7 end}) assert(m.x == 7) end)
probe("coroutines", function() local c = coroutine.wrap(function() coroutine.yield(1) end) assert(c() == 1) end)
probe("pcall/error", function() local ok = pcall(function() error("x") end) assert(not ok) end)
probe("string concatenation", function() local s = "" for i = 1, 200 do s = s .. "a" end assert(#s == 200) end)
probe("big string", function() assert(#string.rep("ab", 2000) == 4000) end)
probe("math library", function() assert(math.floor(1.5) == 1) assert(math.sqrt(4) == 2) end)
probe("math %f path", function() assert(string.format("%.2f", math.pi) == "3.14") end)
probe("io.write", function() io.write("   (io.write ok)\n") end)
probe("io file write/read", function()
  local f = assert(io.open("probe.tmp", "w")) f:write("x") f:close()
  local g = assert(io.open("probe.tmp", "r")) assert(g:read("a") == "x") g:close()
  os.remove("probe.tmp")
end)
probe("os.date/os.time", function() assert(type(os.time()) == "number") assert(#os.date("%Y") >= 4) end)
probe("utf8", function() assert(utf8.len("abc") == 3) end)
probe("recursion depth 200", function() local function d(n) if n == 0 then return 0 end return 1 + d(n-1) end assert(d(200) == 200) end)
probe("deep recursion 2000", function() local function d(n) if n == 0 then return 0 end return 1 + d(n-1) end assert(d(2000) == 2000) end)
probe("garbage collector", function() collectgarbage() local t = {} for i = 1, 2000 do t[i] = {i} end collectgarbage() end)
probe("load/string.dump", function() assert(load("return 1")() == 1) end)
probe("require (app path)", function() assert(type(package.path) == "string") end)

-- the hardware libraries.  They are called through the firmware, so a pcall
-- here says whether the calculator's own services answer, not just whether
-- the binding exists.
probe("gfx.init/size", function()
  assert(gfx.init()) assert(gfx.width() == 320 and gfx.height() == 240)
end)
probe("gfx.draw/getpixel", function()
  gfx.clear(gfx.BLACK) gfx.pixel(3, 3, gfx.RED)
  assert(gfx.getpixel(3, 3) == gfx.RED)
end)
probe("gfx.text/measure", function()
  gfx.text(2, 2, "probe", gfx.WHITE) assert(gfx.textwidth("probe") > 0)
end)
probe("gfx.grob", function()
  local g = gfx.newgrob(8, 8) gfx.select(g) gfx.clear(gfx.MAGENTA)
  gfx.select() gfx.blit(g, 100, 100) gfx.freegrob(g)
end)
probe("key.install/remove", function()
  -- slot patching is ON by default (key.slot is the escape hatch), so this
  -- probe tests the mechanism the way a script actually meets it
  key.install() assert(key.hooked(), "the hook did not take the slot")
  key.remove()
end)
probe("key.slot switch", function()
  -- key.slot only gates the hook's touch-frame processing now; install() and
  -- remove() must round-trip either way
  key.slot(true)
  key.install() assert(key.hooked(), "the hook did not take the slot")
  key.remove()
  key.slot(false)
  key.install() assert(key.hooked(), "install() must still work")
  key.remove()
end)
probe("sys.memory", function() assert(sys.memory() > 0) end)
probe("rand", function()
  rand.seed(7) local a = rand.u32() rand.seed(7) assert(rand.u32() == a)
end)
probe("codec", function()
  assert(codec.crc32("123456789") == 0xCBF43926)
  assert(codec.unhex(codec.hex("hi")) == "hi")
  assert(codec.unbase64(codec.base64("hi")) == "hi")
end)
probe("fx fixed point", function()
  local a = fx.fromdouble(1.5)
  assert(fx.todouble(a) == 1.5) assert(fx.todouble(fx.mul(a, a)) == 2.25)
end)
print("probe done")
"""

def probe():
    return run_source(PROBE, "=probe")

MENU_ENABLED = True
MENU_WIDTH = 62

HELP = """\
  <number>        run that program
  <name>          run a program by name (hello or hello.lua)
  = <lua>         run a Lua one-liner, e.g.  = print(2^10)
  p  probe        self-check every part of the interpreter
  s  selftest     walk the whole launch chain, step by step
  l  log          tail of plua.log (the C side's own diagnostics)
  r  rescan       re-read the folder (after copying a new .lua file)
  n args         run program n, passing the extra words to the script
                 (7 -d  ->  arg[1] == "-d"; see examples/imggesture.lua)
  u  unload       give the interpreter image back to the firmware heap
  g  gc           free memory (the calculator's garbage collector)
  h  help         this text
  q  quit         leave the menu (the terminal goes back to Python)"""

def launcher_log(text):
    try:
        old = b""
        try:
            with uio.FileIO(LAUNCHER_LOG, "rb") as f:
                old = f.read()
        except Exception:
            old = b""
        if len(old) > 8192:
            cut = old.rfind(b"\n", 0, len(old) - 4096)
            old = old[cut + 1:] if cut >= 0 else old[-4096:]
        with uio.FileIO(LAUNCHER_LOG, "wb") as f:
            f.write(old + (text + "\n").encode("utf-8"))
    except Exception:
        pass

def read_launcher_log(n=40):
    try:
        with uio.FileIO(LAUNCHER_LOG, "rb") as f:
            data = f.read()
    except Exception:
        return []
    lines = decode_bytes(data).replace("\r", "").split("\n")
    return [ln for ln in lines if ln][-n:]

def clear_console():
    try:
        if hpprime is not None:
            hpprime.eval("print();")
            return True
    except Exception:
        pass
    return False

def read_line(prompt):
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        return None
    except Exception:
        return None

def menu_show(s):
    p_log("-" * MENU_WIDTH)
    p_log(" primeLua - Lua 5.4.7 on the HP Prime G1")
    p_log(" {}  |  {} script(s)  |  {} KB free".format(
        s.app_dir, len(s.scripts), s.heap_free() // 1024))
    p_log("-" * MENU_WIDTH)
    if not s.scripts:
        p_warning(" [!] no .lua file here -- copy one next to lua.elf")
    for i, it in enumerate(s.scripts):
        p_log("  {:>2}  {:<30} {:>6} B".format(i + 1, it["name"][:30],
                                                it["size"]))
    p_log("-" * MENU_WIDTH)
    p_log("  number/name runs a program   = <lua> one-liner   p probe")
    p_log("  s selftest  l log  r rescan  u unload  h help  q quit")
    p_log("-" * MENU_WIDTH)

def menu_pause(s):
    if read_line("[enter] back to the menu > ") is None:
        return False
    return True

def menu_run(s, name, args=()):
    try:
        run_file(name, args=args)
    except Exception as e:
        p_error("[-] {!r}".format(e))
    for line in s.log_tail():
        if "failed" in line or "FAIL" in line:
            p_warning("[!] " + line)

def menu(script=None):
    try:
        s = session()
    except Exception as e:
        p_error("[-] cannot start: {!r}".format(e))
        return None
    if hpprime is None:
        p_warning("[!] hpprime is not available (not on a calculator)")

    if script:
        clear_console()
        p_log("[*] running {} ...".format(script))
        menu_run(s, script)
        if not menu_pause(s):
            return s.out_lines

    while True:
        clear_console()
        menu_show(s)
        line = read_line("> ")
        if line is None:
            p_log("[*] end of input -- leaving the menu")
            return s.out_lines
        line = line.strip()
        if not line:
            continue
        low = line.lower()
        word, _, rest = low.partition(" ")

        if word in ("q", "quit", "exit", ":q"):
            p_log("[*] bye -- `import main` starts the menu again")
            return s.out_lines
        if word in ("h", "help", "?"):
            clear_console()
            p_log(HELP)
            if not menu_pause(s):
                return s.out_lines
            continue
        if word in ("p", "probe"):
            clear_console()
            menu_run_source(s, PROBE, "=probe")
            if not menu_pause(s):
                return s.out_lines
            continue
        if word in ("s", "selftest"):
            clear_console()
            selftest()
            if not menu_pause(s):
                return s.out_lines
            continue
        if word in ("l", "log"):
            clear_console()
            for ln in s.log_tail()[-40:]:
                p_log("    " + ln)
            if not menu_pause(s):
                return s.out_lines
            continue
        if word in ("r", "rescan", "reload"):
            s.rescan()
            p_log("[*] {} script(s)".format(len(s.scripts)))
            continue
        if word in ("u", "unload"):
            unload()
            p_log("[*] the image is gone; the next run loads it again")
            s = session()
            continue
        if word in ("g", "gc"):
            before = s.heap_free()
            try:
                import gc
                gc.collect()
            except Exception:
                pass
            p_log("[*] {} KB free (was {} KB)".format(s.heap_free() // 1024,
                                                      before // 1024))
            continue
        if word in ("=", "e", "eval", "lua") or line.startswith("="):
            src = line.lstrip("= ").strip() if line.startswith("=") \
                else rest.strip()
            if not src:
                continue
            clear_console()
            menu_run_source(s, src, "=console")
            if not menu_pause(s):
                return s.out_lines
            continue

        it = None
        try:
            n = int(line)
            if 1 <= n <= len(s.scripts):
                it = s.scripts[n - 1]
            else:
                p_error("[-] there is no program {}".format(n))
                continue
        except ValueError:
            name = line if "." in line else line + ".lua"
            for cand in s.scripts:
                if cand["name"].lower() == name.lower():
                    it = cand
                    break
            if it is None:
                p_error("[-] no such program: {}".format(line))
                continue
        clear_console()
        extra = line.split()[1:]
        menu_run(s, it["name"], extra)
        if not menu_pause(s):
            return s.out_lines

def menu_run_source(s, text, name):
    try:
        run_source(text, name)
    except Exception as e:
        p_error("[-] {!r}".format(e))
    for line in s.log_tail():
        if "failed" in line or "FAIL" in line:
            p_warning("[!] " + line)

WIDTH, HEIGHT = 320, 240
GROB = 1

COL_BG = 0xFFFFFF
COL_HEAD = 0x1E3A8A
COL_HEAD_TX = 0xFFFFFF
COL_TX = 0x111111
COL_SUB = 0x6B7280
COL_SEL = 0x3B82F6
COL_SEL_TX = 0xFFFFFF
COL_LINE = 0xCBD5E1
COL_OK = 0x0F6B52
COL_WARN = 0xDC2626
COL_WARN_TX = 0xFFD0D0

GUI_MAX_FRAMES = None
GUI_POLL_MS = 8

ACTIONS = [
    ("probe", "probe / self-test", "38 checks, every feature through pcall"),
    ("selftest", "selftest / launch chain", "debug iface -> heap -> ELF -> run"),
    ("log", "log / plua.log", "the C side's own diagnostics"),
    ("rescan", "rescan folder", "after copying a new .lua in"),
    ("unload", "unload image", "give the resident interpreter back"),
    ("quit", "quit", "back to the Python terminal"),
]

try:
    import time as _time
except Exception:
    _time = None

def _pace(ms=GUI_POLL_MS):
    try:
        fn = getattr(_time, "sleep_ms", None)
        if fn is not None:
            fn(ms)
            return
        fn = getattr(time, "sleep", None)
        if fn is not None:
            fn(ms / 1000.0)
    except Exception:
        pass

class Screen:

    def __init__(self, hp):
        self.hp = hp
        self.th = 14
        self.tw = {}

    def setup(self):
        try:
            self.hp.dimgrob(GROB, WIDTH, HEIGHT, COL_BG)
        except Exception:
            pass
        try:
            r = self.hp.eval('TEXTSIZE("Ag",0)')
            self.th = max(int(r[1]), 12)
        except Exception:
            pass

    def measure(self, ch):
        w = self.tw.get(ch)
        if w is not None:
            return w
        try:
            r = self.hp.eval('TEXTSIZE("%s",0)' % ch.replace('"', "'"))
            w = int(r[0])
            if w <= 0:
                w = 8
        except Exception:
            w = 8 if ord(ch) < 128 else 16
        self.tw[ch] = w
        return w

    def text_w(self, s):
        n = 0
        for ch in s:
            n += self.measure(ch)
        return n

    def fill(self, x, y, w, h, c):
        self.hp.fillrect(GROB, x, y, w, h, c, c)

    def text(self, x, y, s, c):
        if s:
            self.hp.textout(GROB, x, y, s, c)

    def blit(self):
        self.hp.blit(0, 0, 0, GROB)

class KeyPad:

    def __init__(self, hp):
        self.hp = hp
        self.frame = 0
        self.down = set()
        self.latch = None

    def _getkey(self):
        try:
            r = self.hp.eval("GETKEY")
        except Exception:
            return None
        try:
            k = int(r)
        except Exception:
            try:
                k = int(r[0])
            except Exception:
                return None
        return k if k >= 0 else None

    def poll(self):
        self.frame += 1
        cur = set()
        try:
            m = self.hp.keyboard()
        except Exception:
            m = 0
        try:
            m = int(m)
        except Exception:
            m = 0
        for k in range(64):
            if m & (1 << k):
                cur.add(k)
        if not cur:
            gk = self._getkey()
            if gk is None:
                self.latch = None
            elif self.latch == gk:
                gk = None
            else:
                self.latch = gk
            if gk is not None:
                cur.add(gk)
        pressed = sorted(cur - self.down)
        self.down = cur
        return pressed

    def wait(self, max_frames=1200):
        self.poll()
        for _ in range(max_frames):
            if self.poll():
                return True
            _pace()
        return False

class MousePad:

    RAW_W, RAW_H = 320, 240

    def __init__(self, hp):
        self.hp = hp
        self.down = False
        self.last = None

    def _raw(self):
        try:
            m = self.hp.mouse()
        except Exception:
            return None, 0
        if not m:
            return None, 0
        try:
            first = m[0]
        except Exception:
            return None, 0
        if not first:
            return None, 0
        try:
            n = len(m)
        except Exception:
            n = 1
        try:
            return (int(first[0]), int(first[1])), max(1, n)
        except Exception:
            return None, max(1, n)

    def _screen_xy(self, x, y):
        sx = x * WIDTH // self.RAW_W
        sy = y * HEIGHT // self.RAW_H
        return max(0, min(WIDTH - 1, sx)), max(0, min(HEIGHT - 1, sy))

    def sample(self):
        pos, fingers = self._raw()
        if pos is None:
            return None
        return self._screen_xy(pos[0], pos[1])

    def poll(self):
        pos, fingers = self._raw()
        if pos is None:
            self.down = False
            self.last = None
            return None
        here = self._screen_xy(pos[0], pos[1])
        tap = None if self.down else here
        self.down = True
        self.last = here
        return tap

    def drain(self, rounds=40):
        if self._raw()[0] is None:
            self.down = False
            self.last = None
            return
        for _ in range(rounds):
            _pace()
            if self._raw()[0] is None:
                break
        self.down = False
        self.last = None

def selector_items(s):
    items = []
    for it in s.scripts:
        items.append({"kind": "lua", "name": it["name"], "size": it["size"],
                      "sub": "%d B" % it["size"]})
    for kind, label, hint in ACTIONS:
        items.append({"kind": kind, "name": label, "size": 0, "sub": "",
                      "hint": hint})
    return items

def selector_find(items, name):
    for i, it in enumerate(items):
        if it["kind"] == "lua" and it["name"] == name:
            return i
    return 0

def selector_layout(scr):
    head_h = scr.th + 12
    row_h = scr.th + 3
    top_y = head_h + 5
    bottom = HEIGHT - 4
    return head_h, top_y, row_h, max(1, (bottom - top_y) // row_h)

def selector_draw(scr, s, items, sel, top, status):
    scr.fill(0, 0, WIDTH, HEIGHT, COL_BG)
    head_h, top_y, row_h, visible = selector_layout(scr)
    scr.fill(0, 0, WIDTH, head_h, COL_HEAD)
    ty = (head_h - scr.th) // 2
    scr.text(4, ty, "primeLua", COL_HEAD_TX)
    if status:
        try:
            sub, colour = status
        except Exception:
            sub, colour = str(status), COL_HEAD_TX
    else:
        sub, colour = "Lua 5.4.7", COL_HEAD_TX
    w = scr.text_w(sub)
    x = WIDTH - 6 - w
    if x < 4 + scr.text_w("primeLua") + 8:
        while sub and x + scr.text_w(sub) > WIDTH - 4:
            sub = sub[:-1]
            x = WIDTH - 6 - scr.text_w(sub)
    scr.text(x, ty, sub, colour)
    scr.fill(0, head_h, WIDTH, 1, COL_LINE)

    if not items:
        scr.text(8, top_y, "no .lua program here", COL_SUB)
        scr.blit()
        return top

    if sel < top:
        top = sel
    if sel >= top + visible:
        top = sel - visible + 1
    top = max(0, min(top, max(0, len(items) - visible)))

    y = top_y
    actions_start = len(s.scripts) if s.scripts else 0
    for i in range(top, min(len(items), top + visible)):
        it = items[i]
        chosen = (i == sel)
        if (s.scripts and it["kind"] != "lua" and i == actions_start
                and not chosen):
            scr.fill(6, y - 2, WIDTH - 12, 1, COL_LINE)
        if chosen:
            scr.fill(4, y - 1, WIDTH - 8, row_h, COL_SEL)
        name_col = COL_SEL_TX if chosen else (
            COL_TX if it["kind"] == "lua" else COL_SUB)
        sub_col = COL_SEL_TX if chosen else COL_SUB
        room = WIDTH - 14 - scr.text_w(it["sub"] or "")
        name = it["name"]
        while name and scr.text_w(name) > room:
            name = name[:-1]
        scr.text(10, y, name, name_col)
        if it["sub"]:
            sx = WIDTH - 8 - scr.text_w(it["sub"])
            scr.text(sx, y, it["sub"], sub_col)
        y += row_h

    if len(items) > visible:
        h = max(12, (HEIGHT - top_y - 4) * visible // len(items))
        track = HEIGHT - 4 - top_y
        off = (track - h) * top // max(1, len(items) - visible)
        scr.fill(WIDTH - 3, top_y + off, 2, h, COL_LINE)
    scr.blit()
    return top

TOUCH_DRAG_SLOP = 8

class ListGesture:

    def __init__(self, slop=TOUCH_DRAG_SLOP):
        self.slop = slop
        self.start = None
        self.y0 = None
        self.sel0 = None
        self.moved = False

    def update(self, pos, sel, row_h, count):
        if pos is None:
            if self.start is None:
                return None, None
            start, moved = self.start, self.moved
            self.start = None
            self.y0 = self.sel0 = None
            self.moved = False
            if moved:
                return None, None
            return start, None
        if self.start is None:
            self.start = pos
            self.y0 = pos[1]
            self.sel0 = sel
            self.moved = False
            return None, None
        dy = pos[1] - self.y0
        if not self.moved and abs(dy) <= self.slop:
            return None, None
        self.moved = True
        rows = dy // max(1, row_h)
        if not rows:
            return None, None
        new_sel = self.sel0 - rows
        return None, max(0, min(count - 1, new_sel))

def selector_hit(scr, items, top, x, y):
    head_h, top_y, row_h, visible = selector_layout(scr)
    if y < top_y:
        return None
    row = (y - top_y) // row_h
    if row < 0 or row >= visible:
        return None
    idx = top + row
    if idx < 0 or idx >= len(items):
        return None
    return idx

def selector_activate(scr, s, pad, kind, items, sel, top, what):
    if kind == "lua":
        clear_console()
        launcher_log("[selector] picked " + what)
        p_log("[*] {} ...".format(what))
        menu_run(s, what)
        p_log("[enter] (any key) back to the list")
        pad.wait()
        return False, items, sel, top, selector_status(s.out_code, s.out_lines)
    quit_, items, sel, top, status = selector_action(
        scr, s, pad, kind, items, sel, top)
    return (quit_ == "quit"), items, sel, top, status

def selector_status(code, lines):
    if code is None:
        return ("load failed", COL_WARN)
    if code == 0:
        return ("exit 0   %d line" % len(lines), COL_HEAD_TX)
    return ("exit %s" % code, COL_WARN_TX)

def selector_action(scr, s, pad, kind, items, sel, top):
    if kind == "quit":
        p_log("[*] leaving the selector -- `import main` starts it again")
        return "quit", items, sel, top, None
    if kind == "rescan":
        s.rescan()
        items = selector_items(s)
        sel = min(sel, len(items) - 1)
        return None, items, sel, top, None
    if kind == "unload":
        unload()
        s = session()
        items = selector_items(s)
        return None, items, 0, 0, None
    if kind == "probe":
        run_console(s, pad, lambda: probe(), "probe")
        return None, items, sel, top, ("probe", COL_HEAD_TX)
    if kind == "selftest":
        run_console(s, pad, lambda: selftest(), "selftest")
        return None, items, sel, top, ("selftest", COL_HEAD_TX)
    if kind == "log":
        run_console(s, pad, run_log, "log")
        return None, items, sel, top, ("log", COL_HEAD_TX)
    return None, items, sel, top, None

def run_log(s=None):
    s = s or session()
    p_log("--- plua.log (interpreter) ---")
    for ln in s.log_tail()[-30:]:
        p_log("    " + ln)
    tail = read_launcher_log(60)
    p_log("--- plua_launcher.log (launcher) ---")
    if not tail:
        p_log("    (nothing yet)")
    for ln in tail:
        p_log("    " + ln)

def run_console(s, pad, fn, what):
    clear_console()
    p_log("[*] {} ...".format(what))
    try:
        fn()
    except Exception as e:
        p_error("[-] {} failed: {!r}".format(what, e))
    p_log("[enter] (any key) back to the list")
    pad.wait()

def selector(script=None):
    try:
        s = session()
    except Exception as e:
        p_error("[-] cannot start: {!r}".format(e))
        return None
    mpad = MousePad(s.hp)
    mpad.drain()
    scr = Screen(s.hp)
    scr.setup()
    pad = KeyPad(s.hp)
    gesture = ListGesture()
    items = selector_items(s)
    sel = selector_find(items, script or DEFAULT_SCRIPT)
    top = 0
    status = None
    frames = 0

    while True:
        top = selector_draw(scr, s, items, sel, top, status)
        acts = pad.poll()
        pos = mpad.sample()
        _row_h = selector_layout(scr)[2]
        tap, dragged = gesture.update(pos, sel, _row_h, len(items))
        if dragged is not None and dragged != sel:
            sel = dragged
            status = None
        frames += 1
        if not acts and tap is None and pos is None:
            if GUI_MAX_FRAMES is not None and frames >= GUI_MAX_FRAMES:
                return s.out_lines
            _pace()
            continue
        if tap is not None:
            hit = selector_hit(scr, items, top, tap[0], tap[1])
            if hit is not None:
                sel = hit
                it = items[sel]
                launcher_log("[selector] tapped " + it["name"])
                _q, items, sel, top, status = selector_activate(
                    scr, s, pad, it["kind"], items, sel, top, it["name"])
                if _q:
                    return s.out_lines
        for k in acts:
            if k == K_UP:
                sel = (sel - 1) % len(items)
            elif k == K_DOWN:
                sel = (sel + 1) % len(items)
            elif k == K_LEFT:
                sel = max(0, sel - 1)
            elif k == K_RIGHT:
                sel = min(len(items) - 1, sel + 1)
            elif k == K_SYMB:
                s.rescan()
                items = selector_items(s)
                sel = min(sel, len(items) - 1)
            elif k == K_HELP:
                run_console(s, pad, probe, "probe")
                status = ("probe", COL_HEAD_TX)
            elif k == K_VIEW:
                run_console(s, pad, run_log, "log")
                status = ("log", COL_HEAD_TX)
            elif k == K_ESC:
                p_log("[*] leaving the selector -- `import main` starts it "
                      "again")
                return s.out_lines
            elif k == K_ENTER:
                it = items[sel]
                _q, items, sel, top, status = selector_activate(
                    scr, s, pad, it["kind"], items, sel, top, it["name"])
                if _q:
                    return s.out_lines
        if GUI_MAX_FRAMES is not None and frames >= GUI_MAX_FRAMES:
            return s.out_lines

def _hex16(b):
    try:
        out = ""
        for i in range(len(b)):
            out += "%02x" % b[i]
        return out
    except Exception:
        return "?"

def _marker_present(name):
    try:
        import os
        if os.environ.get("PLUA_" + name.upper()):
            return True
    except Exception:
        pass
    names = [name]
    for d in APP_DIRS:
        try:
            names.append(d.rstrip("\\") + "\\" + name)
        except Exception:
            pass
    for cand in names:
        try:
            f = uio.FileIO(cand, "rb")
        except Exception:
            continue
        try:
            f.close()
        except Exception:
            pass
        return True
    return False

def main():
    try:
        _tail = read_launcher_log(14)
        if _tail:
            p_log("[i] last session:")
            for _ln in _tail:
                p_log("    " + _ln)
    except Exception:
        pass

    interactive = MENU_ENABLED
    if interactive:
        try:
            if _marker_present("nomenu"):
                interactive = False
        except Exception:
            pass
    if interactive and hpprime is not None:
        return selector(DEFAULT_SCRIPT)

    p_log("[+] primeLua - Lua 5.4.7 on the HP Prime")
    try:
        s = session()
    except Exception as e:
        p_error("[-] cannot start: {!r}".format(e))
        return None
    if not any(it["name"] == DEFAULT_SCRIPT for it in s.scripts):
        p_warning("[!] {} is not there; use main.run_file(\"name.lua\")"
                  .format(DEFAULT_SCRIPT))
    run(DEFAULT_SCRIPT)
    p_log("[*] done -- import main again, or call main.menu()")
    return s.out_lines

if hpprime is not None:
    try:
        import atexit
        atexit.register(_release_at_exit)
    except Exception:
        pass
    main()
    try:
        if getattr(getattr(sys, "implementation", None), "name", "") \
                == "micropython":
            sys.modules.pop(__name__, None)
    except Exception:
        pass
