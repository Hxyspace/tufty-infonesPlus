/* Mapper 88: Namco 118; CHR A16 is tied to PPU A12. */
BYTE Map88_Regs[1]; // Also used by the retained Mapper 154 state hooks.
void Map88_Init()
{
  Map0_Init();
  MapperInit = Map88_Init;
  MapperWrite = Map88_Write;
  Map88_Regs[0] = 0;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  MapBankChr(4, 0x40, 4);
}
void Map88_Write(WORD address, BYTE data)
{
  if ((address & 0xe000) != 0x8000) return;
  if (!(address & 1)) Map88_Regs[0] = data & 7;
  else if (Map88_Regs[0] < 2) MapBankChr(Map88_Regs[0] * 2, data & 0x3e, 2);
  else if (Map88_Regs[0] < 6) MapBankChr(Map88_Regs[0] + 2, (data & 0x3f) | 0x40);
  else MapBankPrg(Map88_Regs[0] - 6, data & 15);
}
