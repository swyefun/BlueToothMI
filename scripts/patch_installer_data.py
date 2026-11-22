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

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for streamed data payload')
