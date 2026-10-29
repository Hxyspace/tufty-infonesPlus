/* Mapper 244: data bit 3 selects PRG/CHR, upper bits select a permutation. */
void Map244_Init()
{
  Map0_Init();
  MapperInit = Map244_Init;
  MapperWrite = Map244_Write;
}
void Map244_Write(WORD, BYTE data)
{
  static const BYTE prg[4][4] = {
    {0,1,2,3}, {3,2,1,0}, {0,2,1,3}, {3,1,2,0}
  };
  static const BYTE chr[8][8] = {
    {0,1,2,3,4,5,6,7}, {0,2,1,3,4,6,5,7},
    {0,1,4,5,2,3,6,7}, {0,4,1,5,2,6,3,7},
    {0,4,2,6,1,5,3,7}, {0,2,4,6,1,3,5,7},
    {7,6,5,4,3,2,1,0}, {7,6,5,4,3,2,1,0}
  };
  if (data & 8) MapBankChr(0, chr[(data >> 4) & 7][data & 7] * 8, 8);
  else MapBankPrg(0, prg[(data >> 4) & 3][data & 3] * 4, 4);
}
