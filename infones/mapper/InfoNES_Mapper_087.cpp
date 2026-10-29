/* Mapper 87: JF boards, swapped low CHR bank bits. */
void Map87_Init()
{
  Map0_Init();
  MapperInit = Map87_Init;
  MapperSram = Map87_Sram;
  MapperSramOwnsWrite = true;
}
void Map87_Sram(WORD address, BYTE data)
{
  if (address >= 0x6000 && address <= 0x7fff)
    MapBankChr(0, (((data & 1) << 1) | ((data & 2) >> 1)) * 8, 8);
}
