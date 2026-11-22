import io
p = 'build/installer/xiaomi-band-11/main.lua'
s = io.open(p, encoding='utf-8').read()
if 'data payload' not in s:
    old2 = (
        '    local payloads = {\n'
        '        {"inbox/" .. CONFIG.token .. ".cmi.next", receipt},\n'
        '        {"inbox/" .. CONFIG.token .. ".ko.next", module},\n'
        '    }')
    new2 = old2 + (
        '\n'
        '    -- BSVM game data payload: the firmware fails to expose large face\n'
        '    -- resources (an 816KB single entry was unreadable on-device while a\n'
        '    -- 24KB .ko was fine), so the face ships bsvm_d00.bin..bsvm_dNN.bin\n'
        '    -- 16KB chunks; reassemble them here.\n'
        '    do\n'
        '        local parts, total, i = {}, 0, 0\n'
        '        while true do\n'
        '            local piece = read_all(string.format("%sbsvm_d%02d.bin", SCRIPT_PATH, i))\n'
        '            if not piece or #piece == 0 then break end\n'
        '            parts[#parts + 1] = piece\n'
        '            total = total + #piece\n'
        '            i = i + 1\n'
        '            if i > 256 then error("游戏数据块过多", 0) end\n'
        '        end\n'
        '        check(total > 100000, "缺少游戏数据（读到 " .. i .. " 块）")\n'
        '        payloads[#payloads + 1] = {"bsvm.bin", table.concat(parts)}\n'
        '    end')
    assert old2 in s, 'payloads anchor missing'
    s = s.replace(old2, new2, 1)
if 'read_big' not in s:
    old3 = (
        '    for _, item in ipairs(payloads) do\n'
        '        local path = "/data/canopus/" .. item[1]\n'
        '        checkpoint("写入：" .. item[1])\n'
        '        write_file(path, item[2])\n'
        '        checkpoint("回读校验：" .. item[1])\n'
        '        check(read_all(path) == item[2], "Staged file verification failed")\n'
        '    end')
    new3 = (
        '    -- large payloads: chunked write + chunked verify (single-shot\n'
        '    -- io.write/io.read of hundreds of KB is not trustworthy here)\n'
        '    local function write_big(path, data)\n'
        '        local f = io.open(path, "wb")\n'
        '        check(f, "无法打开：" .. path)\n'
        '        for off = 1, #data, 65536 do\n'
        '            local ok, result = pcall(f.write, f, data:sub(off, off + 65535))\n'
        '            check(ok and result, "写入失败：" .. path)\n'
        '        end\n'
        '        local closed, close_result = pcall(f.close, f)\n'
        '        check(closed and close_result, "关闭失败：" .. path)\n'
        '    end\n'
        '    local function read_big(path)\n'
        '        local f = io.open(path, "rb")\n'
        '        check(f, "回读打开失败：" .. path)\n'
        '        local acc = {}\n'
        '        while true do\n'
        '            local ok, piece = pcall(f.read, f, 65536)\n'
        '            check(ok, "回读失败：" .. path)\n'
        '            if not piece or #piece == 0 then break end\n'
        '            acc[#acc + 1] = piece\n'
        '        end\n'
        '        pcall(f.close, f)\n'
        '        return table.concat(acc)\n'
        '    end\n'
        '    for _, item in ipairs(payloads) do\n'
        '        local path = "/data/canopus/" .. item[1]\n'
        '        checkpoint("写入：" .. item[1])\n'
        '        if #item[2] > 65536 then\n'
        '            write_big(path, item[2])\n'
        '            checkpoint("回读校验：" .. item[1])\n'
        '            check(read_big(path) == item[2], "Staged file verification failed")\n'
        '        else\n'
        '            write_file(path, item[2])\n'
        '            checkpoint("回读校验：" .. item[1])\n'
        '            check(read_all(path) == item[2], "Staged file verification failed")\n'
        '        end\n'
        '    end')
    assert old3 in s, 'payload loop anchor missing'
    s = s.replace(old3, new3, 1)
if 'data recheck' not in s:
    old4 = '    if result == 8 then\n'
    new4 = (
        '    -- re-verify the game data survived the supervisor install\n'
        '    local okv, data_now = pcall(read_big, "/data/canopus/bsvm.bin")\n'
        '    check(okv and type(data_now) == "string" and #data_now > 100000,\n'
        '        "数据落盘校验失败：/data/canopus/bsvm.bin")\n'
        '    if result == 8 then\n')
    assert old4 in s, 'result anchor missing'
    s = s.replace(old4, new4, 1)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for chunked data payload')
