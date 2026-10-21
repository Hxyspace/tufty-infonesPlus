"""Validate generated resources with firmware readers and merged UF2 address scopes."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

PROJECT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PROJECT/"tools"))
import resource_images as images

def block(family,address,flags,number,total,body):
    b=bytearray(struct.pack("<8I",0x0a324655,0x9e5d5157,flags,address,256,number,total,family)
                +body+bytes(220)+struct.pack("<I",0x0ab16f30))
    if family==0xe48bff57:
        struct.pack_into("<I",b,288,0x9957e304)
    return bytes(b)

def apply_uf2(data, flash):
    """Model A3 BOOTSEL: erase each addressed 4 KiB sector once, program pages."""
    erased = set()
    for at in range(512,len(data),512):
        address = struct.unpack_from("<I",data,at+12)[0]-images.BASE
        sector = address//4096
        if sector not in erased:
            flash[sector*4096:(sector+1)*4096] = b"\xff"*4096
            erased.add(sector)
        flash[address:address+256] = data[at+32:at+288]
    return flash

def maps(output):
    info=json.loads((output/"resource-images.json").read_text())
    return {name:tuple(offset//4096 for offset in part["written_sector_offsets"])
            for name,part in info["partitions"].items()}

with tempfile.TemporaryDirectory(prefix="tufty-resource-test-") as directory:
    root=Path(directory)
    resources=root/"resources"
    shutil.copytree(PROJECT/"resources",resources)
    (resources/"system/assets").mkdir()
    (resources/"system/assets/test.txt").write_text("system image test")
    (resources/"system/assets/zeros.bin").write_bytes(bytes(8192))
    (resources/"system/assets/erased.bin").write_bytes(b"\xff"*8192)
    (resources/"state/state/nested").mkdir()
    (resources/"state/state/nested/test.txt").write_text("state image test")
    (resources/"state/state/nes.json").write_text('{"brightness":63}')
    (resources/"state/state/zeros.bin").write_bytes(bytes(8192))
    (resources/"state/state/erased.bin").write_bytes(b"\xff"*8192)
    firmware=block(0xe48bff57,0x10ffff00,0xa000,0,2,b"\xef"*256)
    firmware+=block(images.FAMILY,images.BASE,0x2000,0,1,b"\x42"*256)
    fw=root/"firmware.uf2";fw.write_bytes(firmware)
    output=root/"output";images.build(fw,output,resources)
    parts={name:(output/(name+".bin")).read_bytes() for name in images.REGIONS}
    used=maps(output)
    for file,selected in (("tufty-nes-with-romfs.uf2",{"rom":parts["rom"]}),("tufty-nes-with-filesystem.uf2",parts)):
        data=(output/file).read_bytes()
        images.validate_variant(data,firmware,selected,used)
        # UF2 doubles payload size. Resource collections can legitimately exceed
        # 1 MiB; require exactly the selected sectors, not a fixed bundle limit.
        expected_bytes=len(firmware)+sum(len(used[name])*16*512 for name in selected)
        assert len(data)==expected_bytes
        for name in selected:
            offset,size=images.REGIONS[name]
            assert len(used[name])*4096<size
        # Every written resource sector must contain all sixteen pages, not just nonzero ones.
        pages={struct.unpack_from("<I",data,at+12)[0] for at in range(512,len(data),512)}
        for name in selected:
            offset,_=images.REGIONS[name]
            for sector in used[name]:
                assert all(images.BASE+offset+sector*4096+page*256 in pages for page in range(16))
        damaged=bytearray(data);damaged[-300]^=1
        try: images.validate_variant(damaged,firmware,selected,used)
        except ValueError: pass
        else: raise AssertionError("corrupted UF2 accepted")
        try: images.validate_variant(data[:-512],firmware,selected,used)
        except ValueError: pass
        else: raise AssertionError("missing UF2 page accepted")
    # Neither erased bytes nor zero bytes may be dropped from allocated ROMFS files.
    rmroot=root/"rm-payloads";rmroot.mkdir()
    (rmroot/"erased.bin").write_bytes(b"\xff"*8192)
    (rmroot/"zeros.bin").write_bytes(bytes(8192))
    rm=images.rm1(rmroot)
    assert len(images.rom_sectors(rm))*4096>=16384
    sparse=(output/"tufty-nes-with-filesystem.uf2").read_bytes()
    flash=apply_uf2(sparse,bytearray(b"\xff"*(16*1024*1024)))
    full=root/"flash.bin";full.write_bytes(flash)
    subprocess.run([sys.argv[1],str(full),"4096","packed"],check=True)
    # Erasing only addressed sectors must work on a dirty device as well.
    dirty=apply_uf2(sparse,bytearray(b"\xa5"*(16*1024*1024)))
    full.write_bytes(dirty)
    subprocess.run([sys.argv[1],str(full),"4096","packed"],check=True)
    # Replacing an existing valid filesystem must hide old files/settings too.
    defaults=root/"defaults"
    images.build(fw,defaults,PROJECT/"resources")
    default_uf2=(defaults/"tufty-nes-with-filesystem.uf2").read_bytes()
    full.write_bytes(apply_uf2(default_uf2,dirty))
    subprocess.run([sys.argv[1],str(full),"defaults","packed"],check=True)
    # ROMFS-only updates must preserve existing System/State bytes exactly.
    rom_only=(defaults/"tufty-nes-with-romfs.uf2").read_bytes()
    before=bytes(dirty[0x300000:])
    apply_uf2(rom_only,dirty)
    assert bytes(dirty[0x300000:])==before
    # Empty RM1, odd/even record lengths, and overflow.
    empty=root/"empty";empty.mkdir()
    assert images.rm1(empty).startswith(b"\xd2\xcd\x31")
    (empty/"huge").write_bytes(bytes(1024*1024))
    try: images.rm1(empty)
    except ValueError: pass
    else: raise AssertionError("oversized RM1 accepted")
    (empty/"huge").unlink()
    (empty/"link").symlink_to(PROJECT/"README.md")
    try: images.rm1(empty)
    except ValueError: pass
    else: raise AssertionError("symlink accepted")
print("Resource images: actual RM1/FAT/LittleFS mounts, writable State, UF2 scopes and corruption checks passed")
