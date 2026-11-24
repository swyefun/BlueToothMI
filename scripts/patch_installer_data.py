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
        '    checkpoint("提交签名验证与安装请求")\n')
    assert anchor2 in s, 'submit checkpoint anchor missing'
    s = s.replace(anchor2, new2, 1)

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

# 4. on-screen log viewer buttons (head/tail of /data/canopus/bsvm_log.txt).
if 'log viewer' not in s:
    L = []
    a = L.append
    a('-- bsvm log viewer: paged reader over the whole log (O(1) memory)\n')
    a('do\n')
    a('    local LOGP = "/data/canopus/bsvm_log.txt"\n')
    a('    local MARKP = "/data/bsvm_ld.txt"\n')
    a('    local MAXL, CHUNK, LINEMAX = 13, 1600, 56\n')
    a('    local cur, size = 0, 0\n')
    a('    local page_fwd, page_back, page_last\n')
    a('    local function trim(t)\n')
    a('        if #t <= LINEMAX then return t end\n')
    a('        local i = LINEMAX\n')
    a('        while i > 1 and t:byte(i) >= 128 and t:byte(i) < 192 do i = i - 1 end\n')
    a('        return t:sub(1, i - 1) .. "…"\n')
    a('    end\n')
    a('    local function scan(blob, base, limit)\n')
    a('        local ls, start = {}, 1\n')
    a('        for i = 1, #blob do\n')
    a('            if blob:byte(i) == 10 then\n')
    a('                ls[#ls + 1] = { off = base + start - 1, txt = blob:sub(start, i - 1) }\n')
    a('                start = i + 1\n')
    a('                if limit and #ls >= limit then break end\n')
    a('            end\n')
    a('        end\n')
    a('        return ls, start\n')
    a('    end\n')
    a('    local function readat(path, offset)\n')
    a('        local f = io.open(path, "rb")\n')
    a('        if not f then return nil end\n')
    a('        pcall(f.seek, f, "set", offset)\n')
    a('        local acc, total = {}, 0\n')
    a('        while total < CHUNK do\n')
    a('            local okr, piece = pcall(f.read, f, 1024)\n')
    a('            if not okr or not piece or #piece == 0 then break end\n')
    a('            acc[#acc + 1] = piece\n')
    a('            total = total + #piece\n')
    a('        end\n')
    a('        pcall(f.close, f)\n')
    a('        return table.concat(acc)\n')
    a('    end\n')
    a('    local function getsize()\n')
    a('        local f = io.open(LOGP, "rb")\n')
    a('        if not f then return nil end\n')
    a('        local ok, sz = pcall(f.seek, f, "end", 0)\n')
    a('        pcall(f.close, f)\n')
    a('        if not ok or not sz or sz == 0 then return nil end\n')
    a('        size = sz\n')
    a('        return sz\n')
    a('    end\n')
    a('    local function kb(n) return math.floor(n / 1024) end\n')
    a('    local function render(tag, ls)\n')
    a('        local parts = { tag }\n')
    a('        for i = 1, #ls do parts[#parts + 1] = trim(ls[i].txt) end\n')
    a('        status:set { text = table.concat(parts, string.char(10)), text_color = 0xBFD9FF }\n')
    a('    end\n')
    a('    local function empty()\n')
    a('        status:set { text = "日志为空", text_color = 0xFF9A9A }\n')
    a('    end\n')
    a('    local function show_mark()\n')
    a('        local txt = readat(MARKP, 0)\n')
    a('        if not txt or #txt == 0 then\n')
    a('            status:set { text = "【启动标记】无文件（未跑过）", text_color = 0xFF9A9A }\n')
    a('            return\n')
    a('        end\n')
    a('        status:set { text = "【启动标记】" .. string.char(10) ..\n')
    a('            "1=open 2=size 3=hdr 4=parse" .. string.char(10) ..\n')
    a('            "5=evt 6=db 7=dir 8=loaded" .. string.char(10) ..\n')
    a('            "11=strtab 12=ce 13=maps" .. string.char(10) ..\n')
    a('            "14=troop 9=done" .. string.char(10) ..\n')
    a('            "最后标记: " .. txt, text_color = 0xBFD9FF }\n')
    a('    end\n')
    a('    page_fwd = function()\n')
    a('        local sz = getsize()\n')
    a('        if not sz then empty() return end\n')
    a('        if cur >= size then page_last() return end\n')
    a('        local blob = readat(LOGP, cur)\n')
    a('        local ls = scan(blob, cur, MAXL)\n')
    a('        if #ls == 0 then\n')
    a('            cur = size\n')
    a('            render("【日志】末页 " .. kb(size) .. "KB", { { txt = blob } })\n')
    a('            return\n')
    a('        end\n')
    a('        local nxt = ls[#ls].off + #ls[#ls].txt + 1\n')
    a('        render("【日志】" .. kb(cur) .. "/" .. kb(size) .. "KB", ls)\n')
    a('        cur = nxt\n')
    a('    end\n')
    a('    page_back = function()\n')
    a('        local sz = getsize()\n')
    a('        if not sz then empty() return end\n')
    a('        if cur <= 0 then cur = 0 page_fwd() return end\n')
    a('        local target = cur - CHUNK\n')
    a('        if target < 0 then target = 0 end\n')
    a('        local blob = readat(LOGP, target)\n')
    a('        local ls = scan(blob, target, nil)\n')
    a('        if target > 0 and #ls > 0 then table.remove(ls, 1) end\n')
    a('        if #ls == 0 then\n')
    a('            cur = 0\n')
    a('            page_fwd()\n')
    a('            return\n')
    a('        end\n')
    a('        local first = (#ls > MAXL) and (#ls - MAXL + 1) or 1\n')
    a('        local keep = {}\n')
    a('        for i = first, #ls do keep[#keep + 1] = ls[i] end\n')
    a('        cur = keep[1].off\n')
    a('        render("【日志】" .. kb(cur) .. "/" .. kb(size) .. "KB", keep)\n')
    a('    end\n')
    a('    page_last = function()\n')
    a('        local sz = getsize()\n')
    a('        if not sz then empty() return end\n')
    a('        local target = size - CHUNK\n')
    a('        if target < 0 then target = 0 end\n')
    a('        local blob = readat(LOGP, target)\n')
    a('        local ls, pstart = scan(blob, target, nil)\n')
    a('        if target > 0 and #ls > 0 then table.remove(ls, 1) end\n')
    a('        if pstart and pstart <= #blob then\n')
    a('            ls[#ls + 1] = { off = target + pstart - 1, txt = blob:sub(pstart) }\n')
    a('        end\n')
    a('        if #ls == 0 then empty() return end\n')
    a('        local first = (#ls > MAXL) and (#ls - MAXL + 1) or 1\n')
    a('        local keep = {}\n')
    a('        for i = first, #ls do keep[#keep + 1] = ls[i] end\n')
    a('        cur = keep[1].off\n')
    a('        render("【日志】" .. kb(cur) .. "/" .. kb(size) .. "KB", keep)\n')
    a('    end\n')
    a('    local function mkbtn(text, x_ofs, y_ofs, fn)\n')
    a('        local b = lvgl.Object(root, {\n')
    a('            w = (width - 60) / 2, h = 42,\n')
    a('            bg_color = 0x14508A, bg_opa = lvgl.OPA(100), border_width = 0,\n')
    a('            align = { type = lvgl.ALIGN.TOP_MID, x_ofs = x_ofs, y_ofs = y_ofs },\n')
    a('        })\n')
    a('        b:clear_flag(lvgl.FLAG.SCROLLABLE)\n')
    a('        b:add_flag(lvgl.FLAG.CLICKABLE)\n')
    a('        if lvgl.FLAG.EVENT_BUBBLE then b:add_flag(lvgl.FLAG.EVENT_BUBBLE) end\n')
    a('        lvgl.Label(b, {\n')
    a('            text = text, text_color = 0xFFFFFF,\n')
    a('            text_font = lvgl.Font("MiSans-Regular", 16),\n')
    a('            align = lvgl.ALIGN.CENTER,\n')
    a('        })\n')
    a('        b:onevent(lvgl.EVENT.CLICKED, function() fn() end)\n')
    a('    end\n')
    a('    local w2 = (width - 60) / 2\n')
    a('    mkbtn("标记", -w2 / 2 - 4, height - 178, show_mark)\n')
    a('    mkbtn("末页", w2 / 2 + 4, height - 178, page_last)\n')
    a('    mkbtn("上一页", -w2 / 2 - 4, height - 128, page_back)\n')
    a('    mkbtn("下一页", w2 / 2 + 4, height - 128, page_fwd)\n')
    a('end\n')
    a('\n')
    s = s + '\n' + ''.join(L)

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for streamed data payload + paged log viewer')
