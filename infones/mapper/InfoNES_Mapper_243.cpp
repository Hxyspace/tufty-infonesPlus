/* Mapper 243: Sachen SA-020A register file.
   Open bus is approximated by the address high byte, as elsewhere in core. */
static BYTE Map243_Select, Map243_Regs[8];
static BYTE Map243_Read(WORD address)
{
  return (address & 0xc101) == 0x4101
    ? ((address >> 8) & 0xf8) | Map243_Regs[Map243_Select] : address >> 8;
}
static void Map243_Sync()
{
  MapBankPrg(0, (Map243_Regs[5] & 3) * 4, 4);
  const unsigned chr = (Map243_Regs[2] & 1) | ((Map243_Regs[4] & 1) << 1) |
                       ((Map243_Regs[6] & 3) << 2);
  MapBankChr(0, chr * 8, 8);
  const BYTE modes[] = {5, 0, 1, 3};
  InfoNES_Mirroring(modes[(Map243_Regs[7] >> 1) & 3]);
}
void Map243_Init()
{
  Map0_Init();
  MapperInit = Map243_Init;
  MapperApu = Map243_Apu;
  MapperReadApu = Map243_Read;
  MapperSram = Map243_Apu;
  MapperReadSram = Map243_Read;
  MapperSramOwnsWrite = true;
  Map243_Select = 0;
  for (unsigned n = 0; n < 8; ++n) Map243_Regs[n] = 0;
  Map243_Sync();
}
void Map243_Apu(WORD address, BYTE data)
{
  switch (address & 0xc101) {
    case 0x4100: Map243_Select = data & 7; break;
    case 0x4101:
      Map243_Regs[Map243_Select] = data & 7;
      Map243_Sync();
      break;
  }
}
