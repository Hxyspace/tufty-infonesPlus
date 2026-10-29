/* Mapper 140: Jaleco JF-11/14, register at $6000-$7FFF. */
void Map140_Init()
{
  Map0_Init();
  MapperInit = Map140_Init;
  MapperSram = Map140_Sram;
  MapperSramOwnsWrite = true;
}
void Map140_Sram(WORD address, BYTE data)
{
  if (address < 0x6000 || address >= 0x8000) return;
  MapBankPrg(0, ((data >> 4) & 3) * 4, 4);
  MapBankChr(0, (data & 15) * 8, 8);
}
