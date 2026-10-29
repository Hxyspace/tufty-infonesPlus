/* Mapper 97: TAM-S1, fixed lower and switchable upper PRG windows. */
void Map97_Init()
{
  Map0_Init();
  MapperInit = Map97_Init;
  MapperWrite = Map97_Write;
  MapBankPrg(0, (NesHeader.byRomSize - 1) * 2, 2);
  MapBankPrg(2, (NesHeader.byRomSize - 1) * 2, 2);
}
void Map97_Write(WORD, BYTE data)
{
  MapBankPrg(2, (data & 15) * 2, 2);
  const BYTE modes[] = {3, 0, 1, 2};
  InfoNES_Mirroring(modes[data >> 6]);
}
