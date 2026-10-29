/* Mapper 72: JF-17 rising-edge bank latches; sampled speech omitted. */
static BYTE Map72_Latch;
void Map72_Init()
{
  Map0_Init();
  MapperInit = Map72_Init;
  MapperWrite = Map72_Write;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  Map72_Latch = 0;
}
void Map72_Write(WORD address, BYTE data)
{
  data = MapBusConflict(address, data);
  const BYTE rising = data & ~Map72_Latch;
  Map72_Latch = data;
  if (rising & 0x80) MapBankPrg(0, (data & 7) * 2, 2);
  if (rising & 0x40) MapBankChr(0, (data & 15) * 8, 8);
}
