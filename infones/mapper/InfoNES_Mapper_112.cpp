/* Mapper 112: NTDEC banks and mirroring, no MMC3 swap modes or IRQ. */
static BYTE Map112_Select;
void Map112_Init()
{
  Map0_Init();
  MapperInit = Map112_Init;
  MapperWrite = Map112_Write;
  Map112_Select = 0;
  MapBankPrg(0, 0);
  MapBankPrg(1, 0);
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  MapBankChr(0, 0, 2);
  MapBankChr(2, 0, 2);
  for (unsigned n = 4; n < 8; ++n) MapBankChr(n, 0);
  InfoNES_Mirroring(1);
}
void Map112_Write(WORD address, BYTE data)
{
  switch (address & 0xe001) {
    case 0x8000: Map112_Select = data & 7; break;
    case 0xa000:
      if (Map112_Select < 2) MapBankPrg(Map112_Select, data);
      else if (Map112_Select < 4) MapBankChr((Map112_Select - 2) * 2, data & 0xfe, 2);
      else MapBankChr(Map112_Select, data);
      break;
    // CHR A18 is unconnected on the admitted <=256 KiB boards.
    case 0xc000: break;
    case 0xe000: InfoNES_Mirroring(data & 1 ? 0 : 1); break;
  }
}
