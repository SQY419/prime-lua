--# no-compare
--# expect-exit: 0
--# max-insn: 200000000
--[[----------------------------------------------------------------------------
  keydiag.lua -- 按键到底断在哪一环？只报告事实，不做假设。

  真机现象：clock.lua 按 ESC 不退出、gfxdemo 方向键不动 —— 也就是 **Lua 侧
  收不到任何按键**。这条链有四环，每一环都可能断，而且从屏幕上看不出来：

      key.install()  →  固件槽被改写  →  固件输入线程调用钩子  →  事件进队列
                     →  key.getkey() 取到

  这个脚本把每一环的数字都写进 plua.log（重启不丢），跑一次就能定位：

    install    key.install() 的返回值。false = 钩子没装成（槽那一环就断了）
    hooked     钩子自报的 armed 状态
    calls      固件输入线程调用钩子的次数。**不涨 = 槽没被调用**（槽没改成功，
               或被别人改了）
    L0/L1/L2   入口 / thunk / 处理 三层各自被调用次数。L0 涨而 L1 不涨 = thunk
               选择器没接上；L1 涨而 L2 不涨 = 处理没被调用
    pushed     真正入队的事件数。calls 涨而 pushed 不涨 = 事件被丢（类型不识别）
    full       因队列满丢弃的次数
    bad        类型既不是按键也不是触摸的帧数（>0 = trampoline 没送到帧）
    ev_type    钩子最后看到的原始事件类型（0x00100010 = 按键，15 = 触摸）
    ev_keyword 最后看到的 +28 字（按键时 16 = 按下）
    got        脚本自己从 key.getkey() 取到的按键数

  怎么用：跑起来，**按几下方向键和 ESC**，然后看 plua.log 的末尾，或列表里的
  `log` 行。最后一段报数就是断点。

  60 秒后自动结束（靠帧计数，不用 os 库）。
----------------------------------------------------------------------------]]

local key, sys = key, sys

local function log(s)
  if sys and sys.log then sys.log("keydiag: " .. s) end
end

log("version=" .. tostring(sys.version and sys.version() or "?"))
log("install=" .. tostring(key.install()))
log("hooked=" .. tostring(key.hooked()))

local function snap(tag)
  local h = key.hookstats and key.hookstats() or {}
  log(string.format(
    "%s calls=%s armed=%s slot=%s unarmed=%s bad=%s " ..
    "keys=%s/%s touch=%s/%s/%s frames=%s touching=%s qfull=%s",
    tag,
    tostring(h.calls), tostring(h.armed), tostring(h.slot),
    tostring(h.dropped_unarmed), tostring(h.bad_type),
    tostring(h.key_press), tostring(h.key_release),
    tostring(h.touch_press), tostring(h.touch_moves),
    tostring(h.touch_release), tostring(h.touch_frames),
    tostring(h.touching), tostring(h.queue_full)))
end

snap("boot")

local got, frames = 0, 0
local last_calls = -1
while frames < 3000 do
  frames = frames + 1
  local kc = key.getkey()
  if kc ~= nil then
    got = got + 1
    log(string.format("KEY code=%s (got=%d)", tostring(kc), got))
  end
  local h = key.hookstats and key.hookstats() or {}
  if h.calls ~= last_calls then
    last_calls = h.calls
    snap("tick")
  end
  if got > 0 and got % 5 == 0 then
    snap("got5")
  end
  sys.sleep(20)
end

snap("final")
log("done got=" .. tostring(got) .. " frames=" .. tostring(frames))
