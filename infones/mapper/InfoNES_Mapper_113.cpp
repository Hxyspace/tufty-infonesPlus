/* Mapper 113: NINA-derived latch with mirroring control. */
void Map113_Init()
{
  Map0_Init();
  MapperInit = Map113_Init;
  MapperApu = Map113_Apu;
}
void Map113_Apu(WORD address, BYTE data)
{
  if ((address & 0xe100) != 0x4100) return;
  MapBankPrg(0, ((data >> 3) & 7) * 4, 4);
  MapBankChr(0, ((data & 7) | ((data >> 3) & 8)) * 8, 8);
  InfoNES_Mirroring(data & 0x80 ? 1 : 0);
}
