"""Build temporary test FAT volumes; never reads or writes a physical device."""
import subprocess
import sys
import tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix='tufty-nes-test-') as d:
    root=Path(d)
    image=root/'fat.img'
    image.write_bytes(b'\0'*(12*1024*1024))
    cluster='8' if sys.argv[2]=='512' else '1'
    subprocess.run(['mkfs.fat','--invariant','-S',sys.argv[2],'-s',cluster,'-F','12',str(image)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['mmd','-i',str(image),'::roms'],check=True)
    rom=root/'sample.nes'
    rom.write_bytes(b'NES\x1a'+bytes((1,0,0,0))+b'\0'*8+b'\xea'*16384)
    for name in ('Sample Game.nes','测试.nes'):
        subprocess.run(['mcopy','-i',str(image),str(rom),'::roms/'+name],check=True)
    scenario=sys.argv[3] if len(sys.argv)>3 else ''
    gamepads={
        'gamepad_override':'{"controller_id":456,"ble_enabled":true,"ble_peer":"03:25:20:32:28:98","ble_random":false,"ble_scan_uuid":"0x180f"}',
        'gamepad_no_state':'{"ble_peer":"03:25:20:32:28:98","ble_random":false}'
    }
    if scenario in gamepads:
        config=root/'gamepad.json'
        config.write_text(gamepads[scenario])
        subprocess.run(['mmd','-i',str(image),'::config'],check=True)
        subprocess.run(['mcopy','-i',str(image),str(config),'::config/gamepad.json'],check=True)
    subprocess.run([sys.argv[1],str(image),sys.argv[2],*sys.argv[3:]],check=True)
