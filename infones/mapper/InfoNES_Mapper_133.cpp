/* Mapper 133: Sachen SA72008 latch. */
void Map133_Init()
{
  Map0_Init();
  MapperInit = Map133_Init;
  MapperApu = Map133_Apu;
  MapperWrite = Map133_Apu;
}
void Map133_Apu(WORD address, BYTE data)
{
  if ((address & 0x6100) != 0x4100) return;
  MapBankPrg(0, data & 4, 4);
  MapBankChr(0, (data & 3) * 8, 8);
}
