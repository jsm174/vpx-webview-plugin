#!/usr/bin/env python3
"""Builds a runnable demo table from VPX's blank table and the examples folder.

  tools/demo_table.py /path/to/VPinballX_BGFX.app /tmp/webview-demo

The output folder holds the table (a VPZ folder pack), the example pages next to it and a table.ini that enables
the plugin and embeds the backglass in the playfield window. Play it with:

  VPinballX_BGFX -TableIni <out>/table.ini -Play <out>/table
"""
import json
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def find_binary(app):
    app = pathlib.Path(app)
    if app.suffix == '.app':
        return app / 'Contents/MacOS' / app.stem, app / 'Contents/Resources/assets/blankTable.vpx'
    return app, app.parent / 'assets/blankTable.vpx'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary, blank = find_binary(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    if out.exists():
        shutil.rmtree(out)
    table = out / 'table'
    table.mkdir(parents=True)
    subprocess.run([str(binary), '-Convert', str(blank), str(table)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    flasher = json.loads((table / 'parts/AngleProtractor.json').read_text())
    flasher.update(image='', render_mode=2, image_src_link='ctrl://webview/display?id=1', part_group='', named_layer='Layer_1', height=60.0, modulate_vs_add=0.0)
    for point, (x, y) in zip(flasher['dragpoints'], [(100, 300), (100, 675), (850, 675), (850, 300)]):
        point['center'] = {'x': float(x), 'y': float(y)}
    (table / 'parts/WebViewScreen.json').write_text(json.dumps(flasher, indent=2))
    manifest = json.loads((table / 'table.json').read_text())
    manifest['parts'].append('WebViewScreen')
    (table / 'table.json').write_text(json.dumps(manifest, indent=2))

    script = (table / 'script.vbs').read_text()
    hook = 'Sub Table1_KeyDown(ByVal keycode)\n'
    if hook not in script:
        sys.exit('The blank table script has no Table1_KeyDown to hook')
    script = script.replace(hook, hook + '\tWebViewDemo_KeyDown keycode\n', 1)
    script += '\n' + (REPO / 'examples/webview-demo.vbs').read_text()
    (table / 'script.vbs').write_text(script)

    for page in ('fx', 'overlay', 'sound', 'logic'):
        shutil.copytree(REPO / 'examples' / page, out / page)
    (out / 'table.ini').write_text(
        '[Plugin.WebView]\nEnable = 1\n\n[Backglass]\nBackglassOutput = 2\nBackglassWndX = 0\nBackglassWndY = 0\nBackglassWidth = 640\nBackglassHeight = 512\n')
    print(f'{binary} -TableIni {out / "table.ini"} -Play {table}')


if __name__ == '__main__':
    main()
