/* Mapper 86: JF-13 bank latch. Sampled speech is not emulated. */
void Map86_Init()
{
  Map0_Init();
  MapperInit = Map86_Init;
  MapperSram = Map86_Sram;
  MapperSramOwnsWrite = true;
}
void Map86_Sram(WORD address, BYTE data)
{
  if ((address & 0xf000) != 0x6000) return;
  MapBankPrg(0, ((data >> 4) & 3) * 4, 4);
  MapBankChr(0, ((data & 3) | ((data >> 4) & 4)) * 8, 8);
}
