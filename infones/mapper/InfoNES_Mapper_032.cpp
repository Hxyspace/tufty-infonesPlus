/* Mapper 32: Irem G-101, standard H/V board (submapper 0). */
static BYTE Map32_Mode, Map32_Prg;
static void Map32_SyncPrg()
{
  const unsigned slot = Map32_Mode & 2;
  MapBankPrg(slot, Map32_Prg);
  MapBankPrg(slot ^ 2, (NesHeader.byRomSize << 1) - 2);
}
void Map32_Init()
{
  Map0_Init();
  MapperInit = Map32_Init;
  MapperWrite = Map32_Write;
  Map32_Mode = Map32_Prg = 0;
  Map32_SyncPrg();
  ROMBANK3 = ROMLASTPAGE(0);
}
void Map32_Write(WORD address, BYTE data)
{
  switch (address & 0xf000) {
    case 0x8000:
      Map32_Prg = data & 0x1f;
      Map32_SyncPrg();
      break;
    case 0x9000:
      Map32_Mode = data & 2;
      Map32_SyncPrg();
      InfoNES_Mirroring(data & 1 ? 0 : 1);
      break;
    case 0xa000: MapBankPrg(1, data & 0x1f); break;
    case 0xb000: MapBankChr(address & 7, data); break;
  }
}
