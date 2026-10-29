/* Mapper 76: Namco 3446, four 2 KiB CHR banks. */
static BYTE Map76_Reg;
void Map76_Init()
{
  Map0_Init();
  MapperInit = Map76_Init;
  MapperWrite = Map76_Write;
  Map76_Reg = 0;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
}
void Map76_Write(WORD address, BYTE data)
{
  if ((address & 0xe000) != 0x8000) return;
  if (!(address & 1)) Map76_Reg = data & 7;
  else if (Map76_Reg >= 2 && Map76_Reg <= 5)
    MapBankChr((Map76_Reg - 2) * 2, (data & 0x3f) * 2, 2);
  else if (Map76_Reg >= 6) MapBankPrg(Map76_Reg - 6, data & 0xf);
}
