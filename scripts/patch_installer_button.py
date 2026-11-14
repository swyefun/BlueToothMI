#!/usr/bin/env python3
"""Post-process a generated module-installer watchface main.lua.

The pinned Canopus generator starts the install as soon as the watchface is
opened. This patch replaces that with an explicit "安装模块" button, keeps the
status/progress display, and renders Supervisor diagnostics as signed values
with friendly text for the duplicate/downgrade cases.

Usage: python3 patch_installer_button.py <main.lua> [<main.lua> ...]
Idempotent: an already patched file is left untouched.
"""
import sys
from pathlib import Path

STATUS_OLD = '''local status = lvgl.Label(root, {
    text = "准备安装...", text_color = 0xBFD9FF,
    text_font = lvgl.Font("MiSans-Regular", width < 300 and 14 or 18),
    w = width - 32, h = height - 146,'''
STATUS_NEW = '''local status = lvgl.Label(root, {
    text = "点击「安装模块」开始", text_color = 0xBFD9FF,
    text_font = lvgl.Font("MiSans-Regular", width < 300 and 14 or 18),
    w = width - 32, h = height - 190,'''

DETAIL_OLD = 'local function failure_detail(response) return " error " .. u32(response, 36) end'
DETAIL_NEW = '''local function failure_detail(response)
    local code = u32(response, 36)
    if code >= 0x80000000 then code = code - 0x100000000 end
    if code == -105 then return "：模块已是最新版本，无需重复安装" end
    if code == -109 then return "：已安装版本更新，拒绝降级" end
    return " error " .. code
end'''

TAIL_OLD = '''local ok, message = runner.start(install)
if not ok then fail(message) end'''
TAIL_NEW = '''-- Explicit install button: opening the watchface must not install anything.
local install_button = lvgl.Object(root, {
    w = width - 48, h = 48,
    bg_color = 0x14508A, bg_opa = lvgl.OPA(100), border_width = 0,
    align = { type = lvgl.ALIGN.TOP_MID, x_ofs = 0, y_ofs = height - 68 },
})
install_button:clear_flag(lvgl.FLAG.SCROLLABLE)
install_button:add_flag(lvgl.FLAG.CLICKABLE)
if lvgl.FLAG.EVENT_BUBBLE then install_button:add_flag(lvgl.FLAG.EVENT_BUBBLE) end
lvgl.Label(install_button, {
    text = "安装模块", text_color = 0xFFFFFF,
    text_font = lvgl.Font("MiSans-Regular", 16),
    align = lvgl.ALIGN.CENTER,
})
local install_started = false
install_button:onevent(lvgl.EVENT.CLICKED, function()
    if install_started then return end
    install_started = true
    install_button:clear_flag(lvgl.FLAG.CLICKABLE)
    local ok, message = runner.start(install)
    if not ok then
        install_started = false
        install_button:add_flag(lvgl.FLAG.CLICKABLE)
        fail(message)
    end
end)'''

PAIRS = ((STATUS_OLD, STATUS_NEW), (DETAIL_OLD, DETAIL_NEW), (TAIL_OLD, TAIL_NEW))


def patch(path):
    text = path.read_text(encoding='utf-8')
    if 'install_button' in text:
        return 'already patched'
    for old, new in PAIRS:
        if text.count(old) != 1:
            raise SystemExit('%s: anchor not found exactly once: %r' % (path, old[:56]))
        text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8', newline='\n')
    return 'patched'


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for arg in sys.argv[1:]:
        path = Path(arg)
        if not path.is_file():
            raise SystemExit('missing file: ' + str(path))
        print('%s: %s' % (path, patch(path)))


if __name__ == '__main__':
    main()
