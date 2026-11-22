import io
p = 'build/installer/xiaomi-band-11/main.lua'
s = io.open(p, encoding='utf-8').read()

# 1. streaming helpers right after the write_file definition.
if 'data stream' not in s:
    anchor1 = (
        '        and closed and close_result, "写入失败：" .. path)\n'
        'end\n')
    new1 = anchor1 + (
        '\n'
        '    -- BSVM game data: the firmware cannot expose large face resources\n'
        '    -- (an 816KB single entry was unreadable on-device while a 24KB .ko\n'
        '    -- was fine) and assembling the blob in Lua memory OOM-crashes the\n'
        '    -- watch, so stream 16KB chunks face -> /data one at a time.\n'
        '    local function data_stream_write(log)\n'
        '        local f = io.open("/data/canopus/bsvm.bin", "wb")\n'
        '        check(f, "无法打开：/data/canopus/bsvm.bin")\n'
        '        local total, i = 0, 0\n'
        '        while true do\n'
        '            local piece = read_all(string.format("%sbsvm_d%02d.bin", SCRIPT_PATH, i))\n'
        '            if not piece or #piece == 0 then break end\n'
        '            log("组装写入：块 " .. i)\n'
        '            local ok, result = pcall(f.write, f, piece)\n'
        '            if not ok or not result then\n'
        '                pcall(f.close, f)\n'
        '                error("写入失败（块 " .. i .. "）", 0)\n'
        '            end\n'
        '            total = total + #piece\n'
        '            i = i + 1\n'
        '            if i > 256 then pcall(f.close, f); error("游戏数据块过多", 0) end\n'
        '        end\n'
        '        local closed, close_result = pcall(f.close, f)\n'
        '        check(closed and close_result, "关闭失败：/data/canopus/bsvm.bin")\n'
        '        check(total > 100000, "缺少游戏数据（读到 " .. i .. " 块）")\n'
        '        return total\n'
        '    end\n'
        '    local function data_stream_verify(log)\n'
        '        local f = io.open("/data/canopus/bsvm.bin", "rb")\n'
        '        check(f, "回读打开失败：/data/canopus/bsvm.bin")\n'
        '        local total, i = 0, 0\n'
        '        while true do\n'
        '            local src = read_all(string.format("%sbsvm_d%02d.bin", SCRIPT_PATH, i))\n'
        '            if not src or #src == 0 then break end\n'
        '            log("回读校验：块 " .. i)\n'
        '            local ok, dst = pcall(f.read, f, #src)\n'
        '            if not ok or not dst or #dst ~= #src then\n'
        '                pcall(f.close, f)\n'
        '                error("回读失败（块 " .. i .. "）", 0)\n'
        '            end\n'
        '            if dst ~= src then\n'
        '                pcall(f.close, f)\n'
        '                error("数据不一致（块 " .. i .. "）", 0)\n'
        '            end\n'
        '            total = total + #src\n'
        '            i = i + 1\n'
        '            if i > 256 then pcall(f.close, f); error("游戏数据块过多", 0) end\n'
        '        end\n'
        '        local ok2, extra = pcall(f.read, f, 1)\n'
        '        pcall(f.close, f)\n'
        '        if extra and #extra > 0 then error("落盘数据多出尾部", 0) end\n'
        '        check(total > 100000, "游戏数据不完整")\n'
        '        return total\n'
        '    end\n')
    assert anchor1 in s, 'write_file anchor missing'
    s = s.replace(anchor1, new1, 1)

# 2. run streaming before the install request is submitted.
if 'data stream call' not in s:
    anchor2 = '    checkpoint("提交签名验证与安装请求")\n'
    new2 = (
        '    checkpoint("组装写入游戏数据")\n'
        '    local data_total = data_stream_write(checkpoint)\n'
        '    checkpoint("回读校验游戏数据")\n'
        '    local ver_total = data_stream_verify(checkpoint)\n'
        '    check(data_total == ver_total, "游戏数据校验不一致")\n'
        '    checkpoint("提交签名验证与安装请求")\n')
    assert anchor2 in s, 'submit checkpoint anchor missing'
    s = s.replace(anchor2, new2, 1)

# 3. post-install recheck: size-only via seek, never load the file.
if 'data recheck' not in s:
    anchor3 = '    if result == 8 then\n'
    new3 = (
        '    -- re-verify the game data survived the supervisor install\n'
        '    do\n'
        '        local f = io.open("/data/canopus/bsvm.bin", "rb")\n'
        '        check(f, "数据落盘校验失败：文件丢失")\n'
        '        local ok, size = pcall(f.seek, f, "end", 0)\n'
        '        pcall(f.close, f)\n'
        '        check(ok and size and size > 100000, "数据落盘校验失败：大小异常")\n'
        '    end\n'
        '    if result == 8 then\n')
    assert anchor3 in s, 'result anchor missing'
    s = s.replace(anchor3, new3, 1)

# 4. on-screen log viewer buttons (head/tail of /data/canopus/bsvm_log.txt).
if 'log viewer' not in s:
    L = []
    a = L.append
    a('\n')
    a('-- bsvm log viewer: bounded head/tail reads, shown on the status label\n')
    a('do\n')
    a('    local LOGP = "/data/canopus/bsvm_log.txt"\n')
    a('    local function read_slice(path, kind, maxb)\n')
    a('        local f = io.open(path, "rb")\n')
    a('        if not f then return nil end\n')
    a('        local ok, size = pcall(f.seek, f, "end", 0)\n')
    a('        if not ok or not size or size == 0 then pcall(f.close, f); return nil end\n')
    a('        local from\n')
    a('        if kind == "tail" then\n')
    a('            from = (size > maxb) and (size - maxb) or 0\n')
    a('        else\n')
    a('            from = 0\n')
    a('        end\n')
    a('        local till = (size < from + maxb) and size or (from + maxb)\n')
    a('        pcall(f.seek, f, "set", from)\n')
    a('        local acc, total = {}, 0\n')
    a('        while total < till - from do\n')
    a('            local okr, piece = pcall(f.read, f, 1024)\n')
    a('            if not okr or not piece or #piece == 0 then break end\n')
    a('            acc[#acc + 1] = piece\n')
    a('            total = total + #piece\n')
    a('        end\n')
    a('        pcall(f.close, f)\n')
    a('        local txt = table.concat(acc)\n')
    a('        if kind == "tail" and size > maxb then\n')
    a('            local i = txt:find("\\n")\n')
    a('            if i then txt = txt:sub(i + 1) end\n')
    a('        end\n')
    a('        return txt\n')
    a('    end\n')
    a('    local function show(kind)\n')
    a('        local txt = read_slice(LOGP, kind, 1100)\n')
    a('        if not txt or #txt == 0 then\n')
    a('            status:set { text = "日志为空", text_color = 0xFF9A9A }\n')
    a('        else\n')
    a('            local tag\n')
    a('            if kind == "tail" then\n')
    a('                tag = "【日志尾部】\\n"\n')
    a('            else\n')
    a('                tag = "【日志头部】\\n"\n')
    a('            end\n')
    a('            status:set { text = tag .. txt, text_color = 0xBFD9FF }\n')
    a('        end\n')
    a('    end\n')
    a('    local function mkbtn(text, x_ofs, kind)\n')
    a('        local b = lvgl.Object(root, {\n')
    a('            w = (width - 56) / 2, h = 44,\n')
    a('            bg_color = 0x14508A, bg_opa = lvgl.OPA(100), border_width = 0,\n')
    a('            align = { type = lvgl.ALIGN.TOP_MID, x_ofs = x_ofs, y_ofs = height - 126 },\n')
    a('        })\n')
    a('        b:clear_flag(lvgl.FLAG.SCROLLABLE)\n')
    a('        b:add_flag(lvgl.FLAG.CLICKABLE)\n')
    a('        if lvgl.FLAG.EVENT_BUBBLE then b:add_flag(lvgl.FLAG.EVENT_BUBBLE) end\n')
    a('        lvgl.Label(b, {\n')
    a('            text = text, text_color = 0xFFFFFF,\n')
    a('            text_font = lvgl.Font("MiSans-Regular", 14),\n')
    a('            align = lvgl.ALIGN.CENTER,\n')
    a('        })\n')
    a('        b:onevent(lvgl.EVENT.CLICKED, function() show(kind) end)\n')
    a('    end\n')
    a('    local half = (width - 56) / 4\n')
    a('    mkbtn("日志头部", -half - 2, "head")\n')
    a('    mkbtn("日志尾部", half + 2, "tail")\n')
    a('end\n')
    s = s + '\n' + ''.join(L)

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for streamed data payload + log viewer')
