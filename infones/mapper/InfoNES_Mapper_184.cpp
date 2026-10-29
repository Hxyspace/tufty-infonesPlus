/* Mapper 184: Sunsoft-1, independent 4 KiB CHR windows. */
void Map184_Init()
{
  Map0_Init();
  MapperInit = Map184_Init;
  MapperSram = Map184_Sram;
  MapperSramOwnsWrite = true;
  Map184_Sram(0x6000, 0);
}
void Map184_Sram(WORD address, BYTE data)
{
  if (address < 0x6000 || address > 0x7fff) return;
  MapBankChr(0, (data & 7) * 4, 4);
  MapBankChr(4, (((data >> 4) & 7) | 4) * 4, 4);
}
