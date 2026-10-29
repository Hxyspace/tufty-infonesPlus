/* Mapper 242: battery-backed Waixing RPG variant only.
   Multicart variants also need PRG-read overrides and CHR write protection. */
void Map242_Init()
{
  Map0_Init();
  MapperInit = Map242_Init;
  MapperWrite = Map242_Write;
  InfoNES_Mirroring(1);
}
void Map242_Write(WORD address, BYTE)
{
  MapBankPrg(0, ((address >> 3) & 15) * 4, 4);
  InfoNES_Mirroring(address & 2 ? 0 : 1);
}
