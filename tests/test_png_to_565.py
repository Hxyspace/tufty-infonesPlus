"""Near-opaque PNG pixels must survive the binary-transparency conversion."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None


@unittest.skipIf(Image is None, "Pillow is required by the optional PNG converter")
class ConversionTests(unittest.TestCase):
    def test_alpha_and_little_endian(self):
        path = Path(__file__).resolve().parents[1] / "tools/png_to_565.py"
        spec = importlib.util.spec_from_file_location("png_to_565", path)
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "icon.png"
            target = source.with_suffix(".565")
            image = Image.new("RGBA", (7, 1))
            image.putdata([(0, 255, 0, alpha) for alpha in (0, 1, 127, 128, 253, 254, 255)])
            image.save(source)
            converter.convert(source, target)
            self.assertEqual(target.stat().st_size, 14)
            self.assertEqual(struct.unpack("<7H", target.read_bytes()),
                             (0xF81F, 0xF81F, 0xF81F, 0x07E0, 0x07E0, 0x07E0, 0x07E0))


if __name__ == "__main__":
    unittest.main()
