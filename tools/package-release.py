
"""Package the tested Kai build and the official MAME 0.287 chdman executable."""
from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile
import os
import re
import shutil
import struct
import subprocess
from zlib import crc32


ROOT = Path(__file__).resolve().parents[1]
MAME_VERSION = "0.287"
MAME_ARCHIVES = {
    "binary": "68cdaf6d48213c6f3d0f7fa7f2733db46f74e400ad66db2d8a8d777430a42fb9",
    "source": "29f204c09759b7ced7701daad8f4c3f2bcd9a2a497b25363dfb12772499824ef",
}
CHDMAN_SHA256 = "e475e5422e17557f38ec8e430e001cf566f377fed353d43c33a9fd8ce70c729d"
RUNTIME_FILES = (
    "CdManipulatorKai.exe",
    "CdManipulator.chm",
    "Japanese.cml",
    "English.cml",
    "copier.cml",
)
SEVEN_ZIP_MAGIC = b"7z\xbc\xaf\x27\x1c"


def file_sha256(path):
    digest = sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_hash(path, expected):
    actual = file_sha256(path)
    if actual != expected:
        raise ValueError(f"{path}: expected SHA-256 {expected}, got {actual}")


def extract_source_archive(source, output):
    # The released source is a Windows self-extracting 7z archive. Remove
    # only its launcher so the unmodified archive can be opened as .7z.
    with source.open("rb") as stream:
        prefix = stream.read(1024 * 1024)
        for found in re.finditer(re.escape(SEVEN_ZIP_MAGIC), prefix):
            offset = found.start()
            header = prefix[offset:offset + 32]
            if len(header) != 32:
                continue
            checksum, next_offset, next_size = struct.unpack_from("<IQQ", header, 8)
            if crc32(header[12:]) != checksum:
                continue
            if offset + 32 + next_offset + next_size > source.stat().st_size:
                continue
            stream.seek(offset)
            with output.open("wb") as archive:
                shutil.copyfileobj(stream, archive)
            return
    raise ValueError(f"{source}: no valid 7z archive found")


def package(args):
    verify_hash(args.mame_bin, MAME_ARCHIVES["binary"])
    verify_hash(args.mame_source, MAME_ARCHIVES["source"])
    header = (ROOT / "src/CDMVersion.h").read_text(encoding="ascii")
    version = re.search(r'^#define APP_VERSION "([^"]+)"$', header, re.MULTILINE).group(1)
    zip_name = f"CD-Manipulator-Kai-{version}-windows-x64.zip"
    source_name = f"MAME-{MAME_VERSION}-source.7z"

    args.out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="package-", dir=args.out) as folder:
        stage = Path(folder)
        mame = stage / "mame"
        subprocess.run(
            [args.seven_zip, "x", "-y", "-bso0", f"-o{mame}", str(args.mame_bin.resolve()),
             "chdman.exe", "COPYING", "docs/legal/*"],
            check=True,
        )
        verify_hash(mame / "chdman.exe", CHDMAN_SHA256)
        license_dir = mame / "docs/legal"
        if not (license_dir / "GPL-2.0").is_file():
            raise ValueError("MAME license texts are missing")

        with ZipFile(stage / zip_name, "w", ZIP_DEFLATED, compresslevel=9) as archive:
            for name in RUNTIME_FILES:
                archive.write(args.kai_dir / name, name)
            archive.write(ROOT / "src/cdm-license.txt", "cdm-license.txt")
            archive.write(mame / "chdman.exe", "chdman.exe")
            archive.write(mame / "COPYING", "licenses/mame/COPYING")
            for license_path in sorted(license_dir.rglob("*")):
                if license_path.is_file():
                    name = license_path.relative_to(mame).as_posix()
                    archive.write(license_path, f"licenses/mame/{name}")
            readme = (ROOT / "README.md").read_bytes()
            archive.writestr("README.md", readme.replace(b"(src/cdm-license.txt)", b"(cdm-license.txt)"))
            archive.write(ROOT / "CHANGELOG.md", "CHANGELOG.md")

        extract_source_archive(args.mame_source, stage / source_name)
        with ZipFile(stage / zip_name) as archive:
            corrupt = archive.testzip()
            if corrupt is not None:
                raise ValueError(f"Archive integrity check failed for {corrupt}")
        for name in (zip_name, source_name):
            output = args.out / name
            os.replace(stage / name, output)
            print(f"{file_sha256(output)}  {output}")


def main():
    default_7z = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "7-Zip/7z.exe"
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--kai-dir", required=True, type=Path, help="Kai runtime files")
    parser.add_argument("--mame-bin", required=True, type=Path, help="official mame0287b_x64.exe")
    parser.add_argument("--mame-source", required=True, type=Path, help="official mame0287s.exe")
    parser.add_argument("--out", required=True, type=Path, help="release output directory")
    parser.add_argument("--seven-zip", default=str(default_7z) if default_7z.is_file() else "7z")
    package(parser.parse_args())


if __name__ == "__main__":
    main()
