#!/usr/bin/env python3
"""Build RM1/FAT/LittleFS resources and explicitly scoped RP2350 UF2 variants."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from check_uf2 import inspect

PROJECT = Path(__file__).resolve().parents[1]
BASE = 0x10000000
REGIONS = {"rom": (0x200000, 0x100000), "system": (0x300000, 0xc00000),
           "state": (0xf00000, 0x100000)}
FAMILY = 0xe48bff59
ERASE_BYTES = 4096

def sector_range(start, end):
    return range(start//ERASE_BYTES, (end+ERASE_BYTES-1)//ERASE_BYTES)

def rom_sectors(data):
    """Keep the entire RM1 record, including erased-looking file contents."""
    cursor = 0
    def decode():
        nonlocal cursor
        value = 0
        for _ in range(5):
            byte = data[cursor]; cursor += 1
            value = (value<<7) | (byte&127)
            if not byte&128:
                return value
        raise ValueError("invalid RM1 length")
    if decode()!=0x14a6b1:
        raise ValueError("invalid RM1 filesystem record")
    length = decode()
    if length>len(data)-cursor:
        raise ValueError("RM1 record exceeds partition")
    return tuple(sector_range(0,cursor+length))

def fat_sectors(data):
    """Retain FAT12 metadata and allocated clusters, never classify by contents."""
    ss = struct.unpack_from("<H",data,11)[0]
    spc = data[13]
    reserved = struct.unpack_from("<H",data,14)[0]
    fats = data[16]
    root_entries,total = struct.unpack_from("<HH",data,17)
    if not total:
        total = struct.unpack_from("<I",data,32)[0]
    fat_size = struct.unpack_from("<H",data,22)[0]
    if (ss not in (512,1024,2048,4096) or not spc or spc&(spc-1) or
            not reserved or not fats or not fat_size or total*ss!=len(data) or
            data[510:512]!=b"\x55\xaa"):
        raise ValueError("invalid FAT image geometry")
    data_start = reserved+fats*fat_size+(root_entries*32+ss-1)//ss
    clusters = (total-data_start)//spc
    if clusters<=0 or clusters>=4085 or ((clusters+1)*3//2+2)>fat_size*ss:
        raise ValueError("expected a valid FAT12 image")
    used = set(sector_range(0,data_start*ss))
    for cluster in range(2,clusters+2):
        at = reserved*ss+cluster*3//2
        entry = int.from_bytes(data[at:at+2],"little")
        entry = entry>>4 if cluster&1 else entry&0xfff
        if entry:  # Includes reserved/bad clusters; only free clusters are omitted.
            start = (data_start+(cluster-2)*spc)*ss
            used.update(sector_range(start,start+spc*ss))
    return tuple(sorted(used))

def entries(root):
    if not root.is_dir():
        raise ValueError(f"missing resource directory: {root}")
    result = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"resource symlinks are not supported: {path}")
        if any(p.startswith(".") for p in path.relative_to(root).parts):
            continue
        if not path.is_dir() and not path.is_file():
            raise ValueError(f"unsupported resource: {path}")
        if any(any(ord(c)<32 or c in '\\:*?"<>|' for c in part)
               for part in path.relative_to(root).parts):
            raise ValueError(f"invalid resource name: {path}")
        result.append(path)
    return result

def varint(value):
    out = [value & 127]
    value >>= 7
    while value:
        out.insert(0, (value & 127) | 128)
        value >>= 7
    return bytes(out)

def record(kind, data):
    return varint(kind) + varint(len(data)) + data

def rm1(root):
    paths = entries(root)
    if any(len(path.relative_to(root).parts)>16 for path in paths):
        raise ValueError("RM1 resource nesting exceeds firmware limit (16)")
    def directory(parent):
        result = b""
        for path in paths:
            if path.parent != parent:
                continue
            name = path.name.encode("utf-8")
            body = varint(len(name)) + name
            body += directory(path) if path.is_dir() else record(2, path.read_bytes())
            result += record(4 if path.is_dir() else 5, body)
        return result
    payload = directory(root)
    # RomStore requires an even end offset. Padding records are legal RM1 metadata.
    for padding in (b"", record(1,b""), record(1,b"\0"), record(1,b"\0\0")):
        image = record(0x14a6b1, payload+padding)
        if len(image)%2==0:
            if len(image)>REGIONS["rom"][1]:
                raise ValueError("RM1 resources exceed 1 MiB")
            return image.ljust(REGIONS["rom"][1],b"\xff")
    raise ValueError("cannot align RM1")

def run(args):
    subprocess.run([str(a) for a in args],check=True)

def fat(root, output):
    paths = entries(root)
    # FAT paths are case insensitive; reject ambiguous trees before mtools can prompt.
    names = [str(p.relative_to(root)).casefold() for p in paths]
    if len(names)!=len(set(names)):
        raise ValueError("case-insensitive duplicate in system resources")
    output.write_bytes(bytes(REGIONS["system"][1]))
    run(["mkfs.fat","--invariant","-S","4096","-s","1","-F","12","-n","TUFTY NES",output])
    for path in paths:
        target = "::/"+path.relative_to(root).as_posix()
        run(["mmd","-i",output,target] if path.is_dir()
            else ["mcopy","-m","-i",output,path,target])

def state(root, output, work):
    paths = entries(root)
    executable = work/"make_state"
    run([os.environ.get("HOST_CC","cc"),"-std=c11","-O2","-DLFS_NO_DEBUG","-DLFS_NO_WARN",
         "-DLFS_NO_ERROR","-I",PROJECT/"lib/littlefs",PROJECT/"tools/make_state.c",
         PROJECT/"lib/littlefs/lfs.c",PROJECT/"lib/littlefs/lfs_util.c","-o",executable])
    args = [executable,output]
    for path in paths:
        args.extend(["/"+path.relative_to(root).as_posix(), "" if path.is_dir() else path])
    run(args)
    touched = output.with_suffix(output.suffix+".blocks").read_bytes()
    if len(touched)!=REGIONS["state"][1]//ERASE_BYTES or any(v not in (0,1) for v in touched):
        raise ValueError("invalid LittleFS allocated-block map")
    return tuple(i for i,value in enumerate(touched) if value)

def combine(firmware, images, used):
    inspect(firmware)  # Reject non-firmware input, overlaps, and unexpected SDK blocks.
    blocks = [bytearray(firmware[i:i+512]) for i in range(512,len(firmware),512)]
    for name, data in images.items():
        offset, size = REGIONS[name]
        if len(data)!=size:
            raise ValueError(f"wrong {name} partition size")
        sectors = used[name]
        if not sectors or tuple(sorted(set(sectors)))!=sectors or any(s<0 or s>=size//ERASE_BYTES for s in sectors):
            raise ValueError(f"invalid {name} sparse sector map")
        for sector in sectors:
            # BOOTSEL erases a sector when first touched: write all 16 pages.
            for at in range(sector*ERASE_BYTES,(sector+1)*ERASE_BYTES,256):
                header = struct.pack("<8I",0x0a324655,0x9e5d5157,0x2000,BASE+offset+at,
                                     256,0,0,FAMILY)
                blocks.append(bytearray(header+data[at:at+256]+bytes(220)+struct.pack("<I",0x0ab16f30)))
    for i, block in enumerate(blocks):
        struct.pack_into("<2I",block,20,i,len(blocks))
    # Preserve the SDK's separately numbered, tagged E10 compatibility block.
    return firmware[:512]+b"".join(blocks)

def validate_variant(data, firmware, images, used):
    expected = {struct.unpack_from("<I",firmware,i+12)[0]:firmware[i+32:i+288]
                for i in range(512,len(firmware),512)}
    for name, raw in images.items():
        offset,size = REGIONS[name]
        for sector in used[name]:
            expected.update({BASE+offset+i:raw[i:i+256]
                             for i in range(sector*ERASE_BYTES,(sector+1)*ERASE_BYTES,256)})
    if len(data)!=(len(expected)+1)*512 or data[:512]!=firmware[:512]:
        raise ValueError("variant block count/compatibility mismatch")
    for number,i in enumerate(range(512,len(data),512)):
        block=data[i:i+512]
        magic0,magic1,flags,address,size,n,total,family=struct.unpack_from("<8I",block)
        if ((magic0,magic1,flags,size,n,total,family)!=(0x0a324655,0x9e5d5157,0x2000,256,number,len(data)//512-1,FAMILY)
                or struct.unpack_from("<I",block,508)[0]!=0x0ab16f30
                or expected.pop(address,None)!=block[32:288]):
            raise ValueError("variant payload/address/header mismatch")
    if expected:
        raise ValueError("missing variant blocks")

def build(firmware_path, destination, resources):
    destination.mkdir(parents=True,exist_ok=True)
    firmware=firmware_path.read_bytes()
    inspect(firmware)
    with tempfile.TemporaryDirectory(prefix="tufty-images-") as directory:
        work=Path(directory)
        images={"rom":rm1(resources/"rom")}
        fat(resources/"system",work/"system.bin")
        state_used = state(resources/"state",work/"state.bin",work)
        images.update({name:(work/(name+".bin")).read_bytes() for name in ("system","state")})
        used = {"rom":rom_sectors(images["rom"]), "system":fat_sectors(images["system"]),
                "state":state_used}
        variants={"tufty-nes-with-romfs.uf2":{"rom":images["rom"]},
                  "tufty-nes-with-filesystem.uf2":images}
        manifest={}
        for name,parts in variants.items():
            data=combine(firmware,parts,used)
            validate_variant(data,firmware,parts,used)
            (destination/name).write_bytes(data)
            manifest[name]={"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),
                            "replaces":["firmware",*parts], "sparse":True}
        for name,raw in images.items():
            (destination/(name+".bin")).write_bytes(raw)
        (destination/"resource-images.json").write_text(json.dumps({
            "variants":manifest, "partitions":{name:{"offset":REGIONS[name][0],
            "bytes":len(raw),"sha256":hashlib.sha256(raw).hexdigest(),
            "written_bytes":len(used[name])*ERASE_BYTES,
            "written_sector_offsets":[s*ERASE_BYTES for s in used[name]]} for name,raw in images.items()}
            },indent=2)+"\n")
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("firmware",type=Path)
    p.add_argument("destination",type=Path)
    p.add_argument("--resources",type=Path,default=PROJECT/"resources")
    a=p.parse_args()
    try: build(a.firmware,a.destination,a.resources.resolve())
    except (ValueError,OSError,subprocess.CalledProcessError) as e: p.exit(1,f"Resource build failed: {e}\n")

if __name__=="__main__": main()
