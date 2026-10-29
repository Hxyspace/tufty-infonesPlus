/* Mapper 89: Sunsoft-2 bank latch and single-screen mirroring. */
void Map89_Init()
{
  Map0_Init();
  MapperInit = Map89_Init;
  MapperWrite = Map89_Write;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  InfoNES_Mirroring(3);
}
void Map89_Write(WORD, BYTE data)
{
  MapBankPrg(0, ((data >> 4) & 7) * 2, 2);
  MapBankChr(0, ((data & 7) | ((data >> 4) & 8)) * 8, 8);
  InfoNES_Mirroring(data & 8 ? 2 : 3);
}
