--# no-compare
--# expect-exit: 0
--# max-insn: 200000000
--[[----------------------------------------------------------------------------
  devprobe.lua -- 真机上的输入设备能不能直接读？

  背景：primeLua 的按键/触摸现在是靠改写固件 get_event 槽（0x307FBFA0）拿到的，
  而那个槽在真机上反复出问题。PrimeShim（https://github.com/Natsuki-Min/PrimeShim）
  给出了另一条路：这台机器是 Linux，输入设备就在

      /dev/input/event0   键盘（矩阵键盘，16 字节非标准事件结构）
      /dev/input/event1   触摸（标准 Linux input 协议：EV_ABS /
                          ABS_MT_SLOT / ABS_MT_TRACKING_ID / ABS_MT_POSITION_X/Y，
                          以 SYN_REPORT 组帧；原始坐标 X 0..447、Y 0..319，
                          屏是 320x240，所以映射时要缩放）

  如果真机允许我们打开这两个设备，输入就再也不需要碰固件内存了。这个脚本
  只做**只读探测**：尝试以几种路径打开，把结果写进 plua.log。它不写内存、
  不改任何状态，最坏情况是打不开。

  怎么用：跑起来，然后按几个键、碰一下屏幕（让设备产生数据），退出后看
  plua.log 末尾的 `devprobe:` 行。
----------------------------------------------------------------------------]]

local sys = sys

local function log(s)
  if sys and sys.log then sys.log("devprobe: " .. s) end
end

local PATHS = {
  "/dev/input/event0",
  "/dev/input/event1",
  "\\dev\\input\\event0",
  "\\dev\\input\\event1",
}

log("start, io=" .. tostring(io ~= nil))

for _, path in ipairs(PATHS) do
  local ok, f = pcall(io.open, path, "rb")
  if not ok then
    log(string.format("open %-22s -> ERROR %s", path, tostring(f)))
  elseif not f then
    log(string.format("open %-22s -> nil (not found / not allowed)", path))
  else
    log(string.format("open %-22s -> OK", path))
    -- a read on a device with nothing pending would block; try one small read
    -- and report what came back.  pcall protects the script either way.
    local rok, data = pcall(f.read, f, 16)
    if not rok then
      log(string.format("  read -> ERROR %s", tostring(data)))
    elseif data == nil then
      log("  read -> nil")
    else
      log(string.format("  read %d byte(s): %s", #data, tostring(data:byte(1, 16))))
    end
    pcall(f.close, f)
  end
end

-- /proc may or may not be there; it would list the input devices by name
local ok, f = pcall(io.open, "/proc/bus/input/devices", "rb")
if ok and f then
  local d = f:read(400) or ""
  f:close()
  log("proc devices -> " .. tostring(#d) .. " B: " .. d:gsub("[\r\n]+", " | "):sub(1, 200))
else
  log("proc devices -> unavailable")
end

log("done")
print("devprobe: see plua.log")
