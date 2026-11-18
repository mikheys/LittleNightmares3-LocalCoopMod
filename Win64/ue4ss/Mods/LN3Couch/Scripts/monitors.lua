local M = { displays = {}, pairs = {}, status = "off" }
local path = "ue4ss/Mods/LN3Couch/native/monitors_"

local function read(name)
  local f = io.open(path .. name .. ".txt", "r")
  if not f then return "" end
  local text = f:read("*a")
  f:close()
  return text
end

function M.load()
  if M.loaded then return M.scanNative and M.applyNative and M.restoreNative and M.resumeNative end
  M.loaded = true
  local dll = path .. "helper.dll"
  M.scanNative = package.loadlib(dll, "monitor_scan")
  M.applyNative = package.loadlib(dll, "monitor_apply")
  M.restoreNative = package.loadlib(dll, "monitor_restore")
  M.resumeNative = package.loadlib(dll, "monitor_resume")
  return M.scanNative and M.applyNative and M.restoreNative and M.resumeNative
end

function M.scan()
  M.displays, M.pairs = {}, {}
  if not M.load() then M.status = "helper"; return end
  os.remove(path .. "list.txt")
  M.scanNative()
  for line in read("list"):gmatch("[^\r\n]+") do
    local id, x, y, w, h, primary = line:match("^display (%d+) (-?%d+) (-?%d+) (%d+) (%d+) (%d)$")
    if id then
      M.displays[#M.displays + 1] = { id = tonumber(id), x = tonumber(x), y = tonumber(y),
                                    w = w, h = h, primary = primary == "1" }
    else
      local a, b, project = line:match("^pair (%d+) (%d+) (%d)$")
      if a then
        a, b = tonumber(a), tonumber(b)
        M.pairs[a] = M.pairs[a] or {}
        M.pairs[b] = M.pairs[b] or {}
        M.pairs[a][b], M.pairs[b][a] = project == "1", project == "1"
      end
    end
  end
  table.sort(M.displays, function(a, b)
    if a.x ~= b.x then return a.x < b.x end
    return a.y < b.y
  end)
end

local function compatible(a, b)
  return M.pairs[a] and M.pairs[a][b] ~= nil
end

local function partner(id)
  local origin
  for _, d in ipairs(M.displays) do if d.id == id then origin = d; break end end
  if not origin then return end
  for _, d in ipairs(M.displays) do
    if compatible(id, d.id) and not M.pairs[id][d.id] and d.primary then return d.id end
  end
  for _, d in ipairs(M.displays) do
    if compatible(id, d.id) and not M.pairs[id][d.id] and (d.x > origin.x or d.y > origin.y) then return d.id end
  end
  for _, d in ipairs(M.displays) do
    if compatible(id, d.id) and not M.pairs[id][d.id] then return d.id end
  end
  for _, d in ipairs(M.displays) do
    if compatible(id, d.id) then return d.id end
  end
end

function M.defaults(cfg)
  if cfg.monitor_p1 or cfg.monitor_p2 then return end
  for _, d in ipairs(M.displays) do
    local other = d.primary and partner(d.id)
    if other then cfg.monitor_p1, cfg.monitor_p2 = d.id, other; return end
  end
  for _, d in ipairs(M.displays) do
    local other = partner(d.id)
    if other then cfg.monitor_p1, cfg.monitor_p2 = d.id, other; return end
  end
end

function M.cycle(cfg, player, dir)
  local own = player == 1 and "monitor_p1" or "monitor_p2"
  local other = player == 1 and "monitor_p2" or "monitor_p1"
  local options, index = {}, 0
  for _, d in ipairs(M.displays) do
    if M.pairs[d.id] then
      options[#options + 1] = d.id
      if d.id == cfg[own] then index = #options end
    end
  end
  if #options == 0 then return end
  local nextId = options[(index - 1 + dir) % #options + 1]
  if not compatible(nextId, cfg[other]) then
    cfg[other] = compatible(nextId, cfg[own]) and cfg[own] or partner(nextId)
  end
  cfg[own] = nextId
end

function M.label(id)
  for _, d in ipairs(M.displays) do
    if d.id == id then return string.format("DISPLAY%d  %s×%s%s", d.id, d.w, d.h, d.primary and " ★" or "") end
  end
  return id and ("DISPLAY" .. id .. " —") or "—"
end

function M.sync(cfg, coop, shared, time, mask, singleMenu)
  if not (cfg.monitor_enabled and coop) then
    if M.request then M.restoreNative(); M.request = nil end
    M.active, M.shared, M.status = nil, nil, cfg.monitor_enabled and "ready" or "off"
    M.paused, M.manualPause, M.focusPauseUntil, M.seenBlur = nil, nil, nil, nil
    M.wasFocused, M.resumeSent, M.usedDual = nil, nil, nil
    return
  end
  if not M.load() then M.status = "helper"; return end
  local p1, p2 = math.tointeger(cfg.monitor_p1), math.tointeger(cfg.monitor_p2)
  if not p1 or not p2 or p1 < 1 or p2 < 1 or p1 == p2 then
    if M.request then M.restoreNative(); M.request = nil end
    M.active, M.status = nil, "pair"
    return
  end
  if not shared then M.usedDual = true end
  local sharedMode = shared and ((cfg.monitor_shared == "dark" and 1 or 2) + (M.usedDual and not singleMenu and 2 or 0)) or 0
  local req = string.format("%d %d %d %d", p1, p2, sharedMode, mask and 1 or 0)
  if M.request == req and time < M.nextCheck then return end
  M.nextCheck = time + (M.active and 0.2 or 1.5)
  if M.request ~= req then
    local f = io.open(path .. "request.txt", "w")
    if not f then M.status = "request"; return end
    f:write(req); f:close()
    M.request = req
  end
  os.remove(path .. "status.txt")
  M.applyNative()
  local reply = read("status")
  local status = reply:match("^%w+") or "window"
  local axis, second, w, h, foreground, blurs = reply:match("^ok (%d) (%d) %d (%d+) (%d+) (%d) (%d+)")
  if status == "minimized" then return end
  M.status = status
  M.active = axis ~= nil
  M.shared = M.active and shared
  M.singleMenu = M.active and singleMenu == true
  M.horizontal, M.p2First = axis == "1", second == "1"
  M.width, M.height = tonumber(w), tonumber(h)
  M.focused, M.blurs = foreground == "1", tonumber(blurs)
end

function M.shareForPause(paused, forced)
  if paused ~= M.paused then
    M.paused = paused
    M.nextCheck = 0
    if not paused then M.manualPause = false end
  end
  return forced or paused
end

function M.shouldResumeFocusPause(paused, forced, time)
  local newBlur = (M.wasFocused == true and not M.focused) or (M.seenBlur ~= nil and M.seenBlur ~= M.blurs)
  M.seenBlur, M.wasFocused = M.blurs, M.focused
  if newBlur then M.resumeSent = false end
  if (not M.focused or newBlur) and not M.manualPause and not forced then
    M.focusPauseUntil = time + 1.5
  end
  if not paused or M.manualPause or forced then return false end
  if (M.focusPauseUntil or -1) >= time then return M.focused and not M.resumeSent end
  M.manualPause = true
  return false
end

function M.renderFirst(p2First, twoWorlds, holderIsP2)
  if twoWorlds and holderIsP2 ~= nil then return holderIsP2 end
  return p2First
end

return M
