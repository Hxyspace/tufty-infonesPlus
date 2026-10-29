/* Mapper 75: VRC1. No IRQ or delayed register-commit pipeline. */
static BYTE Map75_Chr[2];
void Map75_Init()
{
  Map0_Init();
  MapperInit = Map75_Init;
  MapperWrite = Map75_Write;
  Map75_Chr[0] = 0;
  Map75_Chr[1] = 1;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
}
void Map75_Write(WORD address, BYTE data)
{
  switch (address & 0xf000) {
    case 0x8000: MapBankPrg(0, data); return;
    case 0xa000: MapBankPrg(1, data); return;
    case 0xc000: MapBankPrg(2, data); return;
    case 0x9000:
      if (!ROM_FourScr) InfoNES_Mirroring(data & 1 ? 0 : 1);
      Map75_Chr[0] = (Map75_Chr[0] & 15) | ((data & 2) << 3);
      Map75_Chr[1] = (Map75_Chr[1] & 15) | ((data & 4) << 2);
      break;
    case 0xe000: Map75_Chr[0] = (Map75_Chr[0] & 16) | (data & 15); break;
    case 0xf000: Map75_Chr[1] = (Map75_Chr[1] & 16) | (data & 15); break;
    default: return;
  }
  MapBankChr(0, Map75_Chr[0] * 4, 4);
  MapBankChr(4, Map75_Chr[1] * 4, 4);
}
