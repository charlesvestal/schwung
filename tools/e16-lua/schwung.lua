--[[
  SCHWUNG on the OXI E16, via the device's own Lua API.

  Written against the API reference in the E16 manual (chapter 6), not guessed:
  everything here is namespaced (page.*, controller.*, midi.*, slots.*, leds.*)
  and the signatures are the documented ones.

  WHY THIS EXISTS
  ---------------
  Remote mode draws by shipping a 1024-byte framebuffer -- a 394-packet SysEx
  that must cross four buffers intact, every one of which drops packets
  individually when full. It garbles, and no pacing fixes it (docs/E16_REMOTE.md).

  Here the DEVICE draws and the host sends only what changed:

      page change   ->  0x10  title, 16 labels   ~60 packets, rare
      value change  ->  0x11  index, hi, lo      5 bytes, often

  Nothing on that path can split a message that small.

  WHAT IT CANNOT DO
  -----------------
  Four characters per encoder. The manual is explicit -- `n` writes to
  control.abbr, max 4 chars, and slots.update() truncates the same way. That is
  a property of the DEVICE, not of the transport, so it is the same ceiling
  remote mode's LABELS message has. The 15-character page title is the only
  place a full parameter name fits, so the title follows the encoder under your
  hand.

  WIRE FORMAT
  -----------
  F0 00 21 5B 02 01 <id> <payload> F7 -- the same manufacturer header remote
  mode uses, so a Schwung message is distinguishable from other gear sharing
  the port.

    0x10 PAGE   title \0 lbl1 \0 ... lbl16 \0     7-bit ASCII
    0x11 VALUE  index(0-15) hi(0-127) lo(0-127)   14-bit, matching enc.value
    0x12 TITLE  text                              the focused param, in full
]]

local MFG = { 0x00, 0x21, 0x5B, 0x02, 0x01 }
local MSG_PAGE, MSG_VALUE, MSG_TITLE = 0x10, 0x11, 0x12

local N = 16
local CH_OUT = 0          -- midi.sendCC output index
local CH = 1              -- channel Schwung listens on

local labels = {}
for i = 1, N do labels[i] = "--" end

--[[
  Relative CC, encoded exactly as remote mode sends it.

  src/shared/e16_input.mjs already decodes 0x01..0x08 as clockwise and
  0x7F..0x78 as counter-clockwise, so emitting the same thing means the host
  decoder does not change when the drawing moves onto the device. A second
  encoding would be a second thing to keep in step for no benefit.
]]
local function relativeCC(increment)
  if increment > 0 then return math.min(8, increment) end
  return 128 - math.min(8, -increment)
end

function page.onInit()
  page.setTitle("SCHWUNG")
  -- Sixteen controls the script owns. `manual=true` because the VALUE is the
  -- host's: Schwung holds the parameter and tells us what it became, so the
  -- firmware must not auto-increment underneath that and disagree.
  local controls = {}
  for i = 1, N do
    controls[i] = { i = i, n = labels[i], l = 0, h = 16383, manual = true }
  end
  controller.setControls(controls)
end

function controller.onSysex(bytes)
  -- bytes INCLUDES the F0/F7 framing (manual, 6.5), so the manufacturer id
  -- starts at 2 and the message id sits at 7.
  if type(bytes) ~= "table" or #bytes < 8 then return end
  for i = 1, #MFG do
    if bytes[i + 1] ~= MFG[i] then return end     -- not ours, leave it alone
  end

  local id = bytes[7]
  local p = 8

  if id == MSG_VALUE then
    if #bytes < p + 2 then return end
    local index = bytes[p] + 1                     -- host is 0-based
    local value = bytes[p + 1] * 128 + bytes[p + 2]
    if index >= 1 and index <= N then
      controller.setByIndex(controller.getPage(), index, "v", value)
    end
    return
  end

  if id == MSG_TITLE then
    local cur = {}
    for i = p, #bytes - 1 do cur[#cur + 1] = bytes[i] end
    if #cur > 0 then page.setTitle(string.char(table.unpack(cur))) end
    return
  end

  if id == MSG_PAGE then
    -- NUL-separated: title first, then sixteen labels in order. A short tail
    -- leaves the previous label in place rather than blanking it, so a
    -- truncated message degrades to a stale word instead of an empty grid.
    local fields, cur = {}, {}
    for i = p, #bytes - 1 do
      if bytes[i] == 0 then
        fields[#fields + 1] = string.char(table.unpack(cur)); cur = {}
      else
        cur[#cur + 1] = bytes[i]
      end
    end
    if #cur > 0 then fields[#fields + 1] = string.char(table.unpack(cur)) end

    if fields[1] then page.setTitle(fields[1]) end
    for i = 1, N do
      if fields[i + 1] then
        labels[i] = fields[i + 1]
        slots.update(i, labels[i])                 -- truncates past 4 chars
      end
    end
    return
  end
end

function controller.onEncoderTurn(enc)
  midi.sendCC(CH_OUT, CH, enc.index, relativeCC(enc.increment))
end

function controller.onEncoderPress(enc)
  midi.sendCC(CH_OUT, CH, 64 + enc.index, 127)
end

--[[
  Labels and LED overlays are NOT cleared automatically on a page change
  (manual, 6.5) -- the script owns them. Restating ours is cheaper than
  tracking what was left behind, and it is the same "restate, never mirror"
  rule the Schwung shim uses for pad_block: the other side forgets
  unilaterally and never says so.
]]
function page.onPageChange(prev, curr)
  for i = 1, N do slots.update(i, labels[i]) end
end
