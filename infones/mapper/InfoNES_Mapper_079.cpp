/* Mapper 79: NINA-03/06 register aliases. */
void Map79_Init()
{
  Map0_Init();
  MapperInit = Map79_Init;
  MapperApu = Map79_Apu;
}
void Map79_Apu(WORD address, BYTE data)
{
  if ((address & 0xe100) != 0x4100) return;
  MapBankPrg(0, ((data >> 3) & 1) * 4, 4);
  MapBankChr(0, (data & 7) * 8, 8);
}
