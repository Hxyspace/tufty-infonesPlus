/* Mapper 41: Caltron outer-address/inner-data latch. */
static BYTE Map41_Prg, Map41_Chr;
void Map41_Init()
{
  Map0_Init();
  MapperInit = Map41_Init;
  MapperWrite = Map41_Write;
  MapperSram = Map41_Sram;
  MapperSramOwnsWrite = true;
  Map41_Prg = Map41_Chr = 0;
  InfoNES_Mirroring(1);
}
void Map41_Write(WORD, BYTE data)
{
  if (!(Map41_Prg & 4)) return;
  Map41_Chr = (Map41_Chr & 12) | (data & 3);
  MapBankChr(0, Map41_Chr * 8, 8);
}
void Map41_Sram(WORD address, BYTE)
{
  if (address < 0x6000 || address >= 0x6800) return;
  Map41_Prg = address & 7;
  Map41_Chr = (Map41_Chr & 3) | ((address >> 1) & 12);
  MapBankPrg(0, Map41_Prg * 4, 4);
  MapBankChr(0, Map41_Chr * 8, 8);
  InfoNES_Mirroring(address & 32 ? 0 : 1);
}
