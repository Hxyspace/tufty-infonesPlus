"""Real FAT/LittleFS library lifecycle, with isolated host images only."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="tufty-library-") as temporary:
    root=Path(temporary)
    rom=root/"sample.nes"
    rom.write_bytes(b"NES\x1a"+bytes((1,0,0,0))+bytes(8)+b"\xea"*16384)
    for kind in ("original", "changed", "empty"):
        image=root/(kind+".img")
        image.write_bytes(bytes(12*1024*1024))
        subprocess.run(["mkfs.fat","--invariant","-S","4096","-s","1","-F","12",image],check=True,stdout=subprocess.DEVNULL)
        subprocess.run(["mmd","-i",image,"::roms"],check=True)
        if kind!="empty":
            for name in ("Game10","C","A","Game2","B","D"):
                if kind=="changed" and name=="C": continue
                subprocess.run(["mcopy","-i",image,rom,"::roms/"+name+".nes"],check=True)
            metadata=root/"A.json"
            metadata.write_text(json.dumps({"desc":"First\n\n"+("Edited" if kind=="changed" else "Second")}))
            subprocess.run(["mcopy","-i",image,metadata,"::roms/A.json"],check=True)
    if len(sys.argv)>2 and sys.argv[2]=="power_cut":
        for cut in (0,1,2,3,5,8,13,21,34,55):
            subprocess.run([sys.argv[1],root/"original.img","4096",f"library_cut_{cut}",root],check=True)
            subprocess.run([sys.argv[1],root/"cut.img","4096","library_recover"],check=True)
    elif len(sys.argv)>2 and sys.argv[2]=="limits":
        image=root/"empty.img"
        for i in range(140,0,-1):
            subprocess.run(["mcopy","-i",image,rom,f"::roms/L{i}.nes"],check=True)
            metadata=root/"details.json"
            bad={1:'{"desc":42}',2:'{"name":"Bad","name":"Duplicate"}',3:'{"desc":"\\u0000"}',4:'{"desc":"中文"}',
                 5:'{"desc":'+'['*300+'0'+']'*300+'}',6:json.dumps({"desc":r"literal \u0000 [[[]]]"})}
            metadata.write_text(bad.get(i,json.dumps({"desc":"x"*10000})))
            subprocess.run(["mcopy","-i",image,metadata,f"::roms/L{i}.json"],check=True)
        subprocess.run([sys.argv[1],image,"4096","library_limits"],check=True)
    else:
        subprocess.run([sys.argv[1],root/"original.img","4096","library",root],check=True)
