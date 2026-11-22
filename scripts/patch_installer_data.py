import io
p = 'build/installer/xiaomi-band-11/main.lua'
s = io.open(p, encoding='utf-8').read()
if 'data_name' not in s:
    old = '["assets"] = {},'
    new = old + '\n["data_name"] = "bsvm_data.bin",'
    assert old in s, 'config anchor missing'
    s = s.replace(old, new, 1)
if 'data payload' not in s:
    old2 = (
        '    local payloads = {\n'
        '        {"inbox/" .. CONFIG.token .. ".cmi.next", receipt},\n'
        '        {"inbox/" .. CONFIG.token .. ".ko.next", module},\n'
        '    }')
    new2 = old2 + (
        '\n'
        '    -- BSVM game data payload: flat path (no subdir needed), missing = fatal\n'
        '    do\n'
        '        local ok, bsvm_data = pcall(read_all, SCRIPT_PATH .. (CONFIG.data_name or "bsvm_data.bin"))\n'
        '        if ok and type(bsvm_data) == "string" and #bsvm_data > 1000 then\n'
        '            payloads[#payloads + 1] = {"bsvm.bin", bsvm_data}\n'
        '        else\n'
        '            error("缺少游戏数据 bsvm_data.bin", 0)\n'
        '        end\n'
        '    end')
    assert old2 in s, 'payloads anchor missing'
    s = s.replace(old2, new2, 1)
if 'data recheck' not in s:
    old3 = '    if result == 8 then\n'
    new3 = (
        '    -- re-verify the game data survived the supervisor install\n'
        '    local okv, data_now = pcall(read_all, "/data/canopus/bsvm.bin")\n'
        '    check(okv and type(data_now) == "string" and #data_now > 100000,\n'
        '        "数据落盘校验失败：/data/canopus/bsvm.bin")\n'
        '    if result == 8 then\n')
    assert old3 in s, 'result anchor missing'
    s = s.replace(old3, new3, 1)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('generated installer lua patched for data payload')
