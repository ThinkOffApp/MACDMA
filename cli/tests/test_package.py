import hashlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('verify_package', Path(__file__).resolve().parents[1] / 'tools/verify_package.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageTests(unittest.TestCase):
    def package(self, root, extra=None):
        files = {
            'MCDMACX5Native.kext/Contents/Info.plist': plistlib.dumps({'CFBundleVersion': '0.1.18', 'CFBundleIdentifier': 'org.mcdma.cx5.native'}),
            'MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native': b'nonexecuted synthetic fixture',
            'libmcdma-rdmav34.so': b'synthetic provider', 'mcdma.driver': b'driver /example/libmcdma\n',
        }
        archive = root / 'test.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            for name, data in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
            if extra:
                tar.addfile(extra, io.BytesIO(b''))
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        manifest = {'version': '0.1.18', 'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'files': hashes,
                    'sha256': dict(zip(['kext_plist', 'kext_executable', 'provider', 'conf'], hashes.values()))}
        return archive, manifest

    def test_valid_bounded_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, manifest = self.package(root)
            module.extract(archive, root / 'out', manifest)
            self.assertTrue((root / 'out/mcdma.driver').is_file())

    def test_archive_change_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, manifest = self.package(root)
            archive.write_bytes(archive.read_bytes() + b'changed')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                module.extract(archive, root / 'out', manifest)
            self.assertFalse((root / 'out').exists())

    def test_traversal_and_links_rejected(self):
        for kind in ['traversal', 'symlink', 'hardlink']:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                entry = tarfile.TarInfo('../escape' if kind == 'traversal' else 'link')
                if kind != 'traversal':
                    entry.type = tarfile.SYMTYPE if kind == 'symlink' else tarfile.LNKTYPE
                    entry.linkname = '../escape'
                archive, manifest = self.package(root, entry)
                with self.assertRaises(ValueError):
                    module.extract(archive, root / 'out', manifest)
                self.assertFalse((root / 'escape').exists())

    def test_wrong_bundle_identity_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, manifest = self.package(root)
            manifest['version'] = '0.1.19'
            with self.assertRaisesRegex(ValueError, 'identity'):
                module.extract(archive, root / 'out', manifest)


if __name__ == '__main__':
    unittest.main()
