/* Mapper 92: JF-19 uses JF-17 strobes but switches the upper PRG bank. */
static BYTE Map92_Latch;
void Map92_Init()
{
  Map0_Init();
  MapperInit = Map92_Init;
  MapperWrite = Map92_Write;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  Map92_Latch = 0;
}
void Map92_Write(WORD address, BYTE data)
{
  data = MapBusConflict(address, data);
  const BYTE rising = data & ~Map92_Latch;
  Map92_Latch = data;
  if (rising & 0x80) MapBankPrg(2, (data & 15) * 2, 2);
  if (rising & 0x40) MapBankChr(0, (data & 15) * 8, 8);
}
