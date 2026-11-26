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
        '        local f = io.open("/data/bsvm.bin", "wb")\n'
        '        check(f, "无法打开：/data/bsvm.bin")\n'
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
        '        check(closed and close_result, "关闭失败：/data/bsvm.bin")\n'
        '        check(total > 100000, "缺少游戏数据（读到 " .. i .. " 块）")\n'
        '        return total\n'
        '    end\n'
        '    local function data_stream_verify(log)\n'
        '        local f = io.open("/data/bsvm.bin", "rb")\n'
        '        check(f, "回读打开失败：/data/bsvm.bin")\n'
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
        '    checkpoint("写入图形资源")\n'
        '    asset_stream_write("t", "/data/bsvm_title.bin", 400000, "标题背景", checkpoint)\n'
        '    asset_stream_write("m", "/data/bsvm_map.bin", 300000, "地图", checkpoint)\n'
        '    asset_stream_write("x", "/data/bsvm_box.bin", 80000, "文本框", checkpoint)\n'
        '    asset_stream_write("f", "/data/bsvm_face.bin", 15000, "头像", checkpoint)\n'
        '    asset_stream_write("a", "/data/bsvm_mapdata.bin", 10000, "地图数据", checkpoint)\n'
        '    asset_stream_write("b", "/data/bsvm_seisen_mapdata.bin", 10000, "圣森地图", checkpoint)\n'
        '    checkpoint("提交签名验证与安装请求")\n')
    assert anchor2 in s, 'submit checkpoint anchor missing'
    s = s.replace(anchor2, new2, 1)

# 1b. v0.7 title background: optional 16KB chunk stream bsvm_tNN.bin ->
#     /data/bsvm_title.bin (firmware GPU-decoder ARGB8888 file, see
#     rvdata-tools/make_title.py). Optional: skip silently when the face
#     ships no title chunks (older dist), fail hard on partial/chunk mismatch.
if 'asset streams' not in s:
    anchor1b = (
        '        check(total > 100000, "游戏数据不完整")\n'
        '        return total\n'
        '    end\n')
    new1b = (
        '        check(total > 100000, "游戏数据不完整")\n'
        '        return total\n'
        '    end\n'
        '    -- bsvm asset streams: generic 16KB chunk reader\n'
        '    -- (prefix, dest path, min bytes, label) with round-trip verify\n'
        '    local function asset_stream_write(prefix, dest, min_bytes, label, log)\n'
        '        local first = read_all(string.format("%sbsvm_%s%02d.bin", SCRIPT_PATH, prefix, 0))\n'
        '        if not first or #first == 0 then\n'
        '            log(label .. "缺失，跳过")\n'
        '            return 0\n'
        '        end\n'
        '        local f = io.open(dest, "wb")\n'
        '        check(f, "无法打开：" .. dest)\n'
        '        local total, i = #first, 0\n'
        '        local ok, result = pcall(f.write, f, first)\n'
        '        if not ok or not result then\n'
        '            pcall(f.close, f)\n'
        '            error(label .. "写入失败（块 0）", 0)\n'
        '        end\n'
        '        log(label .. "：块 0")\n'
        '        while true do\n'
        '            i = i + 1\n'
        '            local piece = read_all(string.format("%sbsvm_%s%02d.bin", SCRIPT_PATH, prefix, i))\n'
        '            if not piece or #piece == 0 then break end\n'
        '            log(label .. "：块 " .. i)\n'
        '            local okw, resw = pcall(f.write, f, piece)\n'
        '            if not okw or not resw then\n'
        '                pcall(f.close, f)\n'
        '                error(label .. "写入失败（块 " .. i .. "）", 0)\n'
        '            end\n'
        '            total = total + #piece\n'
        '            if i > 64 then pcall(f.close, f); error(label .. "块过多", 0) end\n'
        '        end\n'
        '        local closed = pcall(f.close, f)\n'
        '        check(closed, "关闭失败：" .. dest)\n'
        '        local vf = io.open(dest, "rb")\n'
        '        check(vf, label .. "回读打开失败")\n'
        '        local rtotal, j = 0, 0\n'
        '        while true do\n'
        '            local srcc = read_all(string.format("%sbsvm_%s%02d.bin", SCRIPT_PATH, prefix, j))\n'
        '            if not srcc or #srcc == 0 then break end\n'
        '            local okr, dst = pcall(vf.read, vf, #srcc)\n'
        '            if not okr or not dst or #dst ~= #srcc or dst ~= srcc then\n'
        '                pcall(vf.close, vf)\n'
        '                error(label .. "校验不一致（块 " .. j .. "）", 0)\n'
        '            end\n'
        '            rtotal = rtotal + #srcc\n'
        '            j = j + 1\n'
        '            if j > 64 then pcall(vf.close, vf); error(label .. "块过多", 0) end\n'
        '        end\n'
        '        local okx, extra_t = pcall(vf.read, vf, 1)\n'
        '        pcall(vf.close, vf)\n'
        '        if extra_t and #extra_t > 0 then error(label .. "文件多出尾部", 0) end\n'
        '        check(rtotal == total and total >= min_bytes, label .. "文件不完整")\n'
        '        log(label .. "写入：" .. total .. " 字节")\n'
        '        return total\n'
        '    end\n')
    assert anchor1b in s, 'title stream anchor missing'
    s = s.replace(anchor1b, new1b, 1)

# 3. post-install recheck: size-only via seek, never load the file.
if 'data recheck' not in s:
    anchor3 = '    if result == 8 then\n'
    new3 = (
        '    -- re-verify the game data survived the supervisor install\n'
        '    do\n'
        '        local f = io.open("/data/bsvm.bin", "rb")\n'
        '        check(f, "数据落盘校验失败：文件丢失")\n'
        '        local ok, size = pcall(f.seek, f, "end", 0)\n'
        '        pcall(f.close, f)\n'
        '        check(ok and size and size > 100000, "数据落盘校验失败：大小异常")\n'
        '    end\n'
        '    if result == 8 then\n')
    assert anchor3 in s, 'result anchor missing'
    s = s.replace(anchor3, new3, 1)

# 4. log viewer (whole log, scrollable) + half-width clear-log button.
if 'clearlog' not in s:
    L = []
    a = L.append
    a('-- bsvm log viewer: whole log in one scrollable view + clear button\n')
    a('do\n')
    a('    local LOGP = "/data/canopus/bsvm_log.txt"\n')
    a('    local loglabel\n')
    a('    local function readlog()\n')
    a('        local f = io.open(LOGP, "rb")\n')
    a('        if not f then return nil end\n')
    a('        local acc = {}\n')
    a('        while true do\n')
    a('            local piece = f:read(4096)\n')
    a('            if not piece or #piece == 0 then break end\n')
    a('            acc[#acc + 1] = piece\n')
    a('        end\n')
    a('        pcall(f.close, f)\n')
    a('        return table.concat(acc)\n')
    a('    end\n')
    a('    local function refresh()\n')
    a('        local blob = readlog()\n')
    a('        if not blob or #blob == 0 then\n')
    a('            status:set { text = "日志为空", text_color = 0xFF9A9A }\n')
    a('            if loglabel then loglabel:set { text = "" } end\n')
    a('            return\n')
    a('        end\n')
    a('        if #blob > 131072 then\n')
    a('            blob = "（仅显示最后 128KB）\\n" .. blob:sub(-131072)\n')
    a('        end\n')
    a('        status:set { text = "" }\n')
    a('        if loglabel then\n')
    a('            loglabel:set { text = blob }\n')
    a('        else\n')
    a('            local box = lvgl.Object(root, {\n')
    a('                w = width - 32, h = height - 190,\n')
    a('                bg_opa = lvgl.OPA(0), border_width = 0,\n')
    a('                align = { type = lvgl.ALIGN.TOP_MID, x_ofs = 0, y_ofs = 106 },\n')
    a('            })\n')
    a('            if lvgl.FLAG.EVENT_BUBBLE then box:add_flag(lvgl.FLAG.EVENT_BUBBLE) end\n')
    a('            loglabel = lvgl.Label(box, {\n')
    a('                text = blob, text_color = 0xBFD9FF,\n')
    a('                text_font = lvgl.Font("MiSans-Regular", width < 300 and 14 or 16),\n')
    a('                w = width - 44,\n')
    a('            })\n')
    a('        end\n')
    a('    end\n')
    a('    refresh()\n')
    a('    -- clear-log button: right half of the bottom row\n')
    a('    local btn_w = (width - 48 - 8) / 2\n')
    a('    local clearlog = lvgl.Object(root, {\n')
    a('        w = btn_w, h = 48,\n')
    a('        bg_color = 0x8A5014, bg_opa = lvgl.OPA(100), border_width = 0,\n')
    a('        align = { type = lvgl.ALIGN.TOP_RIGHT, x_ofs = -24, y_ofs = height - 68 },\n')
    a('    })\n')
    a('    clearlog:clear_flag(lvgl.FLAG.SCROLLABLE)\n')
    a('    clearlog:add_flag(lvgl.FLAG.CLICKABLE)\n')
    a('    if lvgl.FLAG.EVENT_BUBBLE then clearlog:add_flag(lvgl.FLAG.EVENT_BUBBLE) end\n')
    a('    lvgl.Label(clearlog, {\n')
    a('        text = "清空日志", text_color = 0xFFFFFF,\n')
    a('        text_font = lvgl.Font("MiSans-Regular", 16),\n')
    a('        align = lvgl.ALIGN.CENTER,\n')
    a('    })\n')
    a('    clearlog:onevent(lvgl.EVENT.CLICKED, function()\n')
    a('        local f = io.open(LOGP, "wb")\n')
    a('        if f then pcall(f.close, f) end\n')
    a('        refresh()\n')
    a('    end)\n')
    a('end\n')
    s = s + '\n' + ''.join(L)

# 5. install button shrinks to the left half of the bottom row.
if 'clearlog' in s and 'x_ofs = 24, y_ofs = height - 68' not in s:
    anchor5 = (
        'local install_button = lvgl.Object(root, {\n'
        '    w = width - 48, h = 48,\n'
        '    bg_color = 0x14508A, bg_opa = lvgl.OPA(100), border_width = 0,\n'
        '    align = { type = lvgl.ALIGN.TOP_MID, x_ofs = 0, y_ofs = height - 68 },\n'
        '})\n')
    new5 = (
        'local install_button = lvgl.Object(root, {\n'
        '    w = (width - 48 - 8) / 2, h = 48,\n'
        '    bg_color = 0x14508A, bg_opa = lvgl.OPA(100), border_width = 0,\n'
        '    align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 24, y_ofs = height - 68 },\n'
        '})\n')
    assert anchor5 in s, 'install button anchor missing'
    s = s.replace(anchor5, new5, 1)

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for streamed data payload + scrollable log viewer')
