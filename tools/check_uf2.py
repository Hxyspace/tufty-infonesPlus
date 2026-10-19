#!/usr/bin/env python3
"""Check firmware bounds and the SDK's separate, tagged RP2350-E10 block."""
import argparse
from pathlib import Path
import struct

def inspect(data):
    if not data or len(data)%512: raise ValueError('incomplete UF2')
    seen=set()
    firmware=[]
    compatibility=0
    for off in range(0,len(data),512):
        b=data[off:off+512]
        m0,m1,flags,address,size,number,total,family=struct.unpack_from('<8I',b)
        if (m0,m1,struct.unpack_from('<I',b,508)[0])!=(0x0a324655,0x9e5d5157,0x0ab16f30):
            raise ValueError('bad UF2 magic')
        if size!=256 or address%256:
            raise ValueError('expected aligned 256-byte UF2 payloads')
        if family==0xe48bff57:
            if (off!=0 or compatibility or flags!=0xa000 or address!=0x10ffff00 or
                    number!=0 or total!=2 or b[32:288]!=b'\xef'*256 or
                    struct.unpack_from('<I',b,288)[0]!=0x9957e304):
                raise ValueError('unexpected compatibility block')
            compatibility+=1
        elif family==0xe48bff59:
            if flags!=0x2000 or address<0x10000000 or address+size>0x10200000:
                raise ValueError(f'firmware targets storage: {address:#x}')
            if address in seen: raise ValueError('overlapping firmware blocks')
            seen.add(address)
            firmware.append((number,total))
        else: raise ValueError('unexpected family')
    if compatibility!=1 or not firmware: raise ValueError('missing payload or compatibility block')
    if firmware!=[(n,len(firmware)) for n in range(len(firmware))]: raise ValueError('inconsistent block numbering')
    return len(firmware)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('firmware',type=Path)
    a=p.parse_args()
    try: count=inspect(a.firmware.read_bytes())
    except (ValueError,OSError,struct.error) as e: p.error(str(e))
    print(f'UF2 PASS: {count} firmware blocks below 2 MiB; tagged SDK compatibility block at 0x10ffff00')
    print('A3+ Boot ROM ignores the compatibility block; A2 may write the last State sector.')

if __name__=='__main__': main()
