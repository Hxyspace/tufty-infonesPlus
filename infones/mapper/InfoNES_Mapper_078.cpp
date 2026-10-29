/* Mapper 78: explicit JF-16 (1) and Holy Diver (3) wiring.
   Submapper 0 cannot distinguish single-screen from H/V mirroring. */
void Map78_Init()
{
  Map0_Init();
  MapperInit = Map78_Init;
  MapperWrite = Map78_Write;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
}
void Map78_Write(WORD address, BYTE data)
{
  data = MapBusConflict(address, data);
  MapBankPrg(0, (data & 7) * 2, 2);
  MapBankChr(0, (data >> 4) * 8, 8);
  InfoNES_Mirroring(SubMapperNo == 3 ? (data & 8 ? 1 : 0) : (data & 8 ? 2 : 3));
}
