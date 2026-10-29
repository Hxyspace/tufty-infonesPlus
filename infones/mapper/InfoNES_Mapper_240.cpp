/* Mapper 240: expansion-address PRG/CHR latch. */
void Map240_Init()
{
  Map0_Init();
  MapperInit = Map240_Init;
  MapperApu = Map240_Apu;
}
void Map240_Apu(WORD address, BYTE data)
{
  if (address < 0x4020 || address >= 0x6000) return;
  MapBankPrg(0, (data >> 4) * 4, 4);
  MapBankChr(0, (data & 15) * 8, 8);
}
