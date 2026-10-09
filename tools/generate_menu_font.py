#!/usr/bin/env python3
"""Generate the menu's small, hand-drawn 5x7 alphabet and fixed labels. No dependencies."""
from pathlib import Path
# Rows, left to right. These glyphs are maintained here, not imported from a font.
rows = {
'A':['01110','10001','10001','11111','10001','10001','10001'],
'B':['11110','10001','10001','11110','10001','10001','11110'],
'C':['01111','10000','10000','10000','10000','10000','01111'],
'D':['11110','10001','10001','10001','10001','10001','11110'],
'E':['11111','10000','10000','11110','10000','10000','11111'],
'F':['11111','10000','10000','11110','10000','10000','10000'],
'G':['01111','10000','10000','10111','10001','10001','01111'],
'H':['10001','10001','10001','11111','10001','10001','10001'],
'I':['11111','00100','00100','00100','00100','00100','11111'],
'J':['00111','00010','00010','00010','10010','10010','01100'],
'K':['10001','10010','10100','11000','10100','10010','10001'],
'L':['10000','10000','10000','10000','10000','10000','11111'],
'M':['10001','11011','10101','10101','10001','10001','10001'],
'N':['10001','11001','10101','10011','10001','10001','10001'],
'O':['01110','10001','10001','10001','10001','10001','01110'],
'P':['11110','10001','10001','11110','10000','10000','10000'],
'Q':['01110','10001','10001','10001','10101','10010','01101'],
'R':['11110','10001','10001','11110','10100','10010','10001'],
'S':['01111','10000','10000','01110','00001','00001','11110'],
'T':['11111','00100','00100','00100','00100','00100','00100'],
'U':['10001','10001','10001','10001','10001','10001','01110'],
'V':['10001','10001','10001','10001','10001','01010','00100'],
'W':['10001','10001','10001','10101','10101','11011','10001'],
'X':['10001','10001','01010','00100','01010','10001','10001'],
'Y':['10001','10001','01010','00100','00100','00100','00100'],
'Z':['11111','00001','00010','00100','01000','10000','11111'],
'0':['01110','10001','10011','10101','11001','10001','01110'],
'1':['00100','01100','00100','00100','00100','00100','01110'],
'2':['01110','10001','00001','00010','00100','01000','11111'],
'3':['11110','00001','00001','01110','00001','00001','11110'],
'4':['00010','00110','01010','10010','11111','00010','00010'],
'5':['11111','10000','10000','11110','00001','00001','11110'],
'6':['01110','10000','10000','11110','10001','10001','01110'],
'7':['11111','00001','00010','00100','01000','01000','01000'],
'8':['01110','10001','10001','01110','10001','10001','01110'],
'9':['01110','10001','10001','01111','00001','00001','01110'],
'.':['00000','00000','00000','00000','00000','00110','00110'],
'-':['00000','00000','00000','11111','00000','00000','00000'],
':':['00000','00110','00110','00000','00110','00110','00000'],
'[':['01110','01000','01000','01000','01000','01000','01110'],
']':['01110','00010','00010','00010','00010','00010','01110'],
',':['00000','00000','00000','00000','00110','00100','01000'],
'+':['00000','00100','00100','11111','00100','00100','00000'],
'_':['00000','00000','00000','00000','00000','00000','11111'],
'/':['00001','00001','00010','00100','01000','10000','10000'],
}
labels = ['VULKAN FX', 'F10 / ESC TO CLOSE', 'EFFECT ENABLED', 'AO', 'STRENGTH', 'RADIUS', 'BIAS',
          'PSYCHEDELIC', 'RAINBOW', 'HUE FREQUENCY', 'OPACITY', 'DEBUG VIEW', 'NORMAL', 'RAW DEPTH', 'AO ONLY',
          'PSYCHEDELIC', 'SAVE', 'RESET DEFAULTS', 'ARROWS / TAB NAVIGATE - ENTER SELECT',
          'CONFIG MODE - LIVE FILE RELOAD', 'SAVED', 'SAVE FAILED', 'DEFAULTS RESTORED', 'CHANGES APPLY LIVE', 'INPUT SHARED WITH GAME', 'SCENE COLOR - REQUIRES DEPTH_PROBE_SCENE_COLOR', 'DEPTH GRADIENT - CLICK TO CYCLE']
labels += ['NEXT', 'PREVIOUS PAGE', 'MANUAL PROJECTION READY', 'PROJECTION UNAVAILABLE',
           'ADVANCED SETTINGS', 'AUTO UNKNOWN', 'NORMAL Z', 'REVERSED Z', 'UNAVAILABLE',
           'MANUAL PERSPECTIVE', 'MANUAL ORTHOGRAPHIC']
while len(labels)<50: labels.append('')
labels += ['AO LEGACY','RAW DEPTH','AO LEGACY ONLY','FINAL COMPOSITE','SCENE COLOR', 'DEPTH GRADIENT',
           'LINEAR DEPTH','NORMALS','VIEW POSITION Z','SSAO ONLY','SSR HIT MASK','SSR UV','SSR REFLECTION',
           'WATER MASK UNSUPPORTED','VIEW POSITION XYZ']
labels += ['PIPELINE EXPLORER', 'PROTOTYPE - CATEGORIES UNKNOWN', 'CAPTURE 8 FRAMES', 'STOP ISOLATION',
           'ENABLE DEPTH_PROBE_PIPELINE_EXPLORER', 'OFF', 'HIDE SELECTED', 'ONLY SELECTED',
           'ONE TIME DRAWS - AUTO STOP', 'UNKNOWN', 'UNKNOWN TARGET', 'SWAPCHAIN COLOR',
           'OFFSCREEN COLOR', 'DEPTH ONLY', 'MIXED TARGETS']
import re
settings_source=Path(__file__).resolve().parents[1].joinpath('src/overlay_settings.cpp').read_text()
keys=re.findall(r'\{"([a-z_]+)", &OverlaySettings::',settings_source)
diagnostics_source=Path(__file__).resolve().parents[1].joinpath('src/gpu_diagnostics.cpp').read_text()
keys+=re.findall(r'row\(\d+,"([a-z_]+)"',diagnostics_source)
discovery_source=Path(__file__).resolve().parents[1].joinpath('src/discovery_ui.cpp').read_text()
keys+=re.findall(r'row\(\d+,"([a-z_]+)"',discovery_source)
keys=list(dict.fromkeys(keys))
field_labels={}
for key in keys:
    h=2166136261
    for byte in key.encode(): h=((h^byte)*16777619)&0xffffffff
    index=1000+(h&65535)
    assert index not in field_labels
    field_labels[index]=key.replace('_',' ').upper()
output = ['// Generated by tools/generate_menu_font.py; hand-drawn glyphs.\nuint glyphBits(uint c, int bit) {', '    uvec2 bits = uvec2(0);', '    switch (c) {']
for char, shape in rows.items():
    value = sum(int(on) << (y*5+x) for y,row in enumerate(shape) for x,on in enumerate(row))
    output.append(f'    case {ord(char)}u: bits = uvec2({value & 0xffffffff}u, {value >> 32}u); break;')
output += ['    }', '    if (bit < 0 || bit >= 35) return 0u;',
           '    return ((bit < 32 ? bits.x : bits.y) >> (uint(bit) & 31u)) & 1u;', '}']
# Keep fixed labels separate from hashed field IDs. A small sorted lookup avoids
# a large return-valued switch in the fragment shader's divergent text branches.
lookup=sorted((key,index) for index,key in enumerate(field_labels,len(labels)))
output += [f'const ivec2 fieldLabelLookup[{len(lookup)}] = ivec2[](' + ','.join(f'ivec2({key},{index})' for key,index in lookup)+');',
           'int labelIndex(int label) {',
           f'    if(label>=0 && label<{len(labels)})return label;',
           f'    int low=0,high={len(lookup)-1};',
           f'    for(int step=0;step<{len(lookup).bit_length()};++step) {{',
           '        if(low>high)break;',
           '        int middle=(low+high)/2;',
           '        ivec2 entry=fieldLabelLookup[middle];',
           '        if(label==entry.x)return entry.y;',
           '        if(label<entry.x)high=middle-1;else low=middle+1;',
           '    }', '    return -1;', '}']
all_labels=labels+list(field_labels.values())
characters=[]
entries=[]
for label in all_labels:
    entries.append((len(characters),len(label)))
    characters.extend(ord(c) for c in label)
output.append(f'const uvec2 labelRanges[{len(entries)}] = uvec2[](' + ','.join(f'uvec2({offset}u,{length}u)' for offset,length in entries)+');')
output.append(f'const uint labelCharacters[{len(characters)}] = uint[](' + ','.join(f'{c}u' for c in characters)+');')
output += ['uint labelChar(int label, int col) {',
           '    int index=labelIndex(label);',
           f'    if(index<0 || index>={len(entries)} || col<0)return 32u;',
           '    uvec2 range=labelRanges[index];',
           '    if(uint(col)>=range.y)return 32u;',
           '    return labelCharacters[range.x+uint(col)];', '}']
Path(__file__).resolve().parents[1].joinpath('shaders/menu_font.glsl').write_text('\n'.join(output)+'\n')
