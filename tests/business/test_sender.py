import struct
import sys
import unittest
from unittest.mock import Mock,patch
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from serial_gamepad import (FocusedKeyboard,Sender,packet,make_report,keyboard,
    NEUTRAL_REPORT,DISCONNECT_REPORT,A,B,SELECT,START,UP,DOWN,LEFT,RIGHT)
from check_uf2 import inspect

class Wire:
    def __init__(self): self.rx=b''; self.tx=[]
    def write(self,data): self.tx.append(data); return len(data)
    def read(self,n): data,self.rx=self.rx[:n],self.rx[n:]; return data

class Tests(unittest.TestCase):
    def test_foreground_keyboard(self):
        for visible in (True,False):
            with self.subTest(visible_console=visible):
                api=Mock()
                window=0x123456789
                api.kernel32.GetConsoleWindow.return_value=window if visible else 42
                api.user32.IsWindowVisible.return_value=visible
                api.user32.GetForegroundWindow.return_value=window
                pressed={ord('Z'),0x1b}
                api.user32.GetAsyncKeyState.side_effect=lambda key: 0x8000 if key in pressed else 0
                with patch('ctypes.windll',api,create=True): capture=FocusedKeyboard()
                self.assertEqual(capture.window,window)
                self.assertEqual(capture.read(),make_report(A,home=True))
                w=Wire(); sender=Sender(w)
                w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll()
                sender.send(capture.read())
                self.assertEqual(w.tx[-1],packet(1,1,make_report(A,home=True)))
                # Losing focus releases buttons and Home, without ending the session.
                api.user32.GetForegroundWindow.return_value=99
                api.user32.GetAsyncKeyState.reset_mock()
                self.assertEqual(capture.read(),NEUTRAL_REPORT)
                api.user32.GetAsyncKeyState.assert_not_called()
                sender.send(capture.read())
                self.assertEqual(w.tx[-1],packet(1,2,NEUTRAL_REPORT))
                self.assertTrue(sender.ready)
                api.user32.GetForegroundWindow.return_value=window
                pressed={0x0d,0xa1,ord('X')}
                self.assertEqual(capture.read(),make_report(START|SELECT|B))
                api.user32.GetForegroundWindow.side_effect=[window,99]
                self.assertEqual(capture.read(),NEUTRAL_REPORT)

    def test_lost_handshake(self):
        w=Wire(); now=[0.0]; sender=Sender(w,clock=lambda:now[0])
        old=sender.nonce
        now[0]=1.21; sender.poll()
        self.assertFalse(sender.ready)
        self.assertNotEqual(sender.nonce,old)
        self.assertEqual(len(w.tx),2)
        # An old-version READY must never make the v3 sender appear connected.
        w.rx=b'#TGP:READY:'+sender.nonce+b'\n'; sender.poll(); self.assertFalse(sender.ready)
        w.rx=b'#TGP:READY:'+sender.nonce+b':2\n'; sender.poll(); self.assertFalse(sender.ready)
        w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll(); self.assertTrue(sender.ready)

    def test_sender(self):
        w=Wire(); now=[0.0]; sender=Sender(w,clock=lambda:now[0])
        self.assertEqual(w.tx[-1],b'#TGP:HELLO:0001:'+sender.nonce+b':3\n')
        report=make_report(A|RIGHT)
        sender.send(report); self.assertEqual(len(w.tx),1)
        w.rx=b'#TGP:READY:0000000000000000:3\n'; sender.poll(); self.assertFalse(sender.ready)
        w.rx=b'#TGP:READY:'+sender.nonce+b':3\r\n'; sender.poll(); self.assertTrue(sender.ready)
        self.assertEqual(w.tx[-1],packet(1,0,NEUTRAL_REPORT))
        sender.send(report); self.assertEqual(w.tx[-1],packet(1,1,report))
        sender.send(report); self.assertEqual(len(w.tx),3)
        # A duplicate READY cannot inject a neutral frame during gameplay.
        w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll()
        self.assertEqual(len(w.tx),3)
        now[0]=0.12; sender.send(report); self.assertEqual(w.tx[-1],packet(1,2,report))
        # Home is a normal held report, including its release edge.
        sender.send(make_report(home=True)); self.assertEqual(w.tx[-1],packet(1,3,make_report(home=True)))
        sender.send(NEUTRAL_REPORT); self.assertEqual(w.tx[-1],packet(1,4,NEUTRAL_REPORT))
        now[0]=1.0; w.rx=b'#TGP:ALIVE:'+sender.nonce+b'\n'; sender.poll()
        now[0]=1.3; sender.poll(); self.assertTrue(sender.ready)
        old=sender.nonce; now[0]=2.21; sender.poll()
        self.assertFalse(sender.ready); self.assertNotEqual(old,sender.nonce)
        w.rx=b'#TGP:READY:'+old+b':3\n'; sender.poll(); self.assertFalse(sender.ready)
        w.rx=b'x'*200+b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll(); sender.poll(); self.assertFalse(sender.ready)
        w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll(); self.assertTrue(sender.ready)
        old=sender.nonce; w.rx=b'#TGP:BYE:'+old+b'\n'; sender.poll()
        self.assertFalse(sender.ready); self.assertNotEqual(old,sender.nonce)

    def test_close(self):
        for acknowledge in (False,True):
            with self.subTest(acknowledge=acknowledge):
                w=Wire(); sender=Sender(w)
                w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll()
                sender.send(make_report(A))
                if acknowledge: w.rx=b'#TGP:BYE:'+sender.nonce+b'\n'
                sender.close(timeout=0.001)
                self.assertEqual(w.tx[-2:], [packet(1,2,NEUTRAL_REPORT),packet(1,3,DISCONNECT_REPORT)])
                self.assertFalse(sender.ready)
                count=len(w.tx)
                w.rx=b'#TGP:APP\n#TGP:BYE:'+sender.nonce+b'\n'
                sender.poll(); sender.send(make_report(A)); sender.close()
                self.assertEqual(len(w.tx),count) # Intentional close never reopens the session.
        w=Wire(); sender=Sender(w); sender.close(); self.assertEqual(len(w.tx),1)

    def test_report(self):
        self.assertEqual(make_report(),NEUTRAL_REPORT)
        self.assertEqual(make_report(A|RIGHT),bytes.fromhex('80 80 80 80 02 01 00 00 00 00'))
        directions=((UP,0),(UP|RIGHT,1),(RIGHT,2),(DOWN|RIGHT,3),(DOWN,4),
                    (DOWN|LEFT,5),(LEFT,6),(UP|LEFT,7),(UP|DOWN|LEFT|RIGHT,255),
                    (UP|DOWN|RIGHT,2),(LEFT|RIGHT|UP,0))
        for buttons,hat in directions:
            self.assertEqual(make_report(buttons)[4],hat)
        for buttons in range(256):
            report=make_report(buttons,home=True)
            self.assertEqual(len(report),10)
            self.assertEqual(report[:4],b'\x80'*4)
            self.assertEqual(report[5],buttons&3)
            self.assertEqual(report[6],(buttons&12)|16)
            self.assertEqual(report[7:],b'\0'*3)
        pressed={ord('W'),ord('D'),ord('Z'),ord('X'),0x0d,0xa1,0x1b}
        self.assertEqual(keyboard(lambda key: 0x8000 if key in pressed else 0),
                         make_report(UP|RIGHT|A|B|START|SELECT,home=True))
        for buttons in (-1,256,None):
            with self.assertRaises(ValueError): make_report(buttons)

    def test_packet(self):
        self.assertEqual(packet(1,0,make_report(A|RIGHT)),b'#TGP:5447500301000080808080020100000000\n')
        self.assertEqual(packet(1,0,DISCONNECT_REPORT),b'#TGP:54475003010000544750014259450000a5\n')
        self.assertEqual(packet(65535,256,NEUTRAL_REPORT),b'#TGP:54475003ffff0080808080ff0000000000\n')
        for args in ((0,0,NEUTRAL_REPORT),(65536,0,NEUTRAL_REPORT),(1,0,b''),(1,0,b'x'*9),(1,0,b'x'*11),(1,0,10)):
            with self.assertRaises(ValueError): packet(*args)

    def test_legacy_packets(self):
        cases=((0,0),(UP,1),(DOWN,2),(LEFT,4),(RIGHT,8),(A,16),(B,32),
               (SELECT,64),(START,128),(RIGHT|A,0x18),(B|SELECT|START,0xe0),
               (UP|RIGHT,9),(DOWN|RIGHT,10),(DOWN|LEFT,6),(UP|LEFT,5))
        for version in (1,2):
            for buttons,wire_buttons in cases:
                with self.subTest(version=version,buttons=buttons):
                    expected=b'TGP'+bytes((version,0x34,0x12,0,wire_buttons))
                    self.assertEqual(packet(0x1234,256,make_report(buttons),version),
                                     b'#TGP:'+expected.hex().encode()+b'\n')
            self.assertEqual(packet(1,0,make_report(home=True),version),
                             packet(1,0,NEUTRAL_REPORT,version))
        for buttons in range(256):
            report=make_report(buttons)
            v1=packet(1,0,report,1); v2=packet(1,0,report,2)
            self.assertEqual(v1[:11],v2[:11])
            self.assertEqual(v1[11:13],b'01'); self.assertEqual(v2[11:13],b'02')
            self.assertEqual(v1[13:],v2[13:])
        with self.assertRaises(ValueError): packet(1,0,NEUTRAL_REPORT,4)

    def test_legacy_sender(self):
        for version in (1,2):
            with self.subTest(version=version):
                w=Wire(); now=[0.0]
                sender=Sender(w,clock=lambda:now[0],version=version)
                self.assertEqual(w.tx[-1],b'#TGP:HELLO:0001:'+sender.nonce+b'\n')
                w.rx=b'#TGP:READY:'+sender.nonce+b':3\n'; sender.poll()
                self.assertFalse(sender.ready)
                w.rx=b'#TGP:READY:'+sender.nonce+b'\n'; sender.poll()
                self.assertTrue(sender.ready)
                self.assertEqual(w.tx[-1],packet(1,0,NEUTRAL_REPORT,version))
                report=make_report(B|SELECT|START)
                sender.send(report)
                self.assertEqual(w.tx[-1],packet(1,1,report,version))
                count=len(w.tx); sender.send(report); self.assertEqual(len(w.tx),count)
                w.rx=b'#TGP:READY:'+sender.nonce+b'\n'; sender.poll()
                self.assertEqual(len(w.tx),count)
                now[0]=0.12; sender.send(report)
                self.assertEqual(w.tx[-1],packet(1,2,report,version))
                # Focus loss supplies the same neutral report in every mode.
                sender.send(NEUTRAL_REPORT)
                self.assertEqual(w.tx[-1],packet(1,3,NEUTRAL_REPORT,version))
                now[0]=1.0; w.rx=b'#TGP:ALIVE:'+sender.nonce+b'\n'; sender.poll()
                now[0]=1.3; sender.poll(); self.assertTrue(sender.ready)
                old=sender.nonce; now[0]=2.21; sender.poll()
                self.assertFalse(sender.ready); self.assertNotEqual(sender.nonce,old)
                w.rx=b'#TGP:READY:'+old+b'\n'; sender.poll(); self.assertFalse(sender.ready)
                w.rx=b'#TGP:READY:'+sender.nonce+b'\n'; sender.poll()
                w.rx=b'#TGP:BYE:'+sender.nonce+b'\n'; sender.close(timeout=0.001)
                self.assertEqual(w.tx[-2:], [packet(1,1,NEUTRAL_REPORT,version),
                                            b'#TGP:END:'+sender.nonce+b'\n'])
                count=len(w.tx); sender.poll(); sender.send(report); sender.close()
                self.assertFalse(sender.ready); self.assertEqual(len(w.tx),count)

    def test_uf2_storage_rejected(self):
        def block(address,family,flags=0x2000,number=0,total=1):
            b=bytearray(512)
            struct.pack_into('<8I',b,0,0x0a324655,0x9e5d5157,flags,address,256,number,total,family)
            struct.pack_into('<I',b,508,0x0ab16f30)
            if flags==0xa000:
                b[32:288]=b'\xef'*256
                struct.pack_into('<I',b,288,0x9957e304)
            return b
        firmware=block(0x10000000,0xe48bff59)
        compat=block(0x10ffff00,0xe48bff57,0xa000,total=2)
        self.assertEqual(inspect(compat+firmware),1)
        self.assertEqual(inspect(compat+block(0x101fff00,0xe48bff59)),1)
        for address in (0x10200000,0x10300000,0x10ffff00):
            with self.assertRaises(ValueError): inspect(compat+block(address,0xe48bff59))
        invalid=(block(0x101fff00,0xe48bff57,0xa000,total=2)+firmware,
                 block(0x10ffff00,0xe48bff57,0xa000)+firmware,
                 block(0x10ffff00,0xe48bff57,0xa000,number=1,total=2)+firmware,
                 firmware+compat,compat+compat+firmware,
                 compat+block(0x10000000,0xe48bff59,total=2),
                 compat+block(0x10000000,0xe48bff59,number=1),
                 compat+firmware+firmware)
        for data in invalid:
            with self.assertRaises(ValueError): inspect(data)
        for offset in (32,288):
            data=bytearray(compat+firmware); data[offset]^=1
            with self.assertRaises(ValueError): inspect(data)

if __name__=='__main__': unittest.main()
