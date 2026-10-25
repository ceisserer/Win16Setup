#!/usr/bin/env python3
"""Packages a Windows 3.1 product with Win16Setup.

  w16pack.py pack --inf SETUP.INF --files DIR --setup SETUP.EXE --out DIR
                  --name NAME [--label LABEL] [--version X] [--no-floppy]

      Copies the files under --files (DOS 8.3 names) to OUT/NAME/ together
      with SETUP.EXE and SETUP.INF, whose [Files] section is written from
      the files (any [Files] section of the template is replaced; @VERSION@
      is replaced by --version). Then writes OUT/NAME.zip, the 1.44 MB
      floppy image OUT/NAME-disk.img (needs mtools) and OUT/SHA256SUMS.
      Both are reproducible: file dates come from SOURCE_DATE_EPOCH, else
      from the last git commit.

  w16pack.py preview IN OUT.BMP

      Converts an image (e.g. a PNG screenshot, up to about 320 x 200) into
      the 8-bit BMP that SETUP.EXE shows as [Choice] Preview= (needs
      Pillow; must stay below 64 KB).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

DOS_CHARS = r"[A-Z0-9_\-~!#$%&'()@^{}]"


def dos_name_ok(name):
    return re.fullmatch(r"%s{1,8}(\.%s{1,3})?" % (DOS_CHARS, DOS_CHARS), name) is not None


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("latin-1")


def source_date(path):
    if "SOURCE_DATE_EPOCH" in os.environ:
        return int(os.environ["SOURCE_DATE_EPOCH"])
    r = subprocess.run(["git", "-C", path, "log", "-1", "--format=%ct"], capture_output=True, text=True)
    return int(r.stdout.strip() or time.time())


def list_files(root):
    """Relative DOS paths (backslashes) of all files under root."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            rel = os.path.relpath(os.path.join(d, f), root).replace(os.sep, "\\")
            out.append(rel)
    # files in the root first, then by directory
    return sorted(out, key=lambda p: (p.count("\\"), p))


def write_inf(template, files, version, dest):
    with open(template, encoding="latin-1") as f:
        text = f.read().replace("\r\n", "\n")
    if version:
        text = text.replace("@VERSION@", version)
    out, skip = [], False
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("["):
            skip = s.lower() == "[files]"
        if not skip:
            out.append(line)
    while out and not out[-1].strip():
        out.pop()
    out += ["", "[Files]"] + ["%d=%s" % (i + 1, f) for i, f in enumerate(files)] + [""]
    with open(dest, "wb") as f:
        f.write(crlf("\n".join(out)))
    m = re.search(r"^\s*Product\s*=", text, re.M | re.I)
    if not m:
        sys.exit("%s: [Setup] Product= is missing" % template)


def make_zip(root, files, path, epoch):
    t = time.gmtime(epoch)[:6]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            info = zipfile.ZipInfo(f.replace("\\", "/"), date_time=t)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(root, f.replace("\\", os.sep)), "rb") as fh:
                z.writestr(info, fh.read())


def make_floppy(root, files, path, label, epoch):
    for tool in ("mformat", "mcopy", "mmd"):
        if not shutil.which(tool):
            sys.exit("%s not found: install mtools (or use --no-floppy)" % tool)
    # mtools dates new directory entries with SOURCE_DATE_EPOCH
    env = dict(os.environ, MTOOLS_NO_VFAT="1", TZ="UTC", SOURCE_DATE_EPOCH=str(epoch))
    # a zeroed image (mformat -C leaves garbage in the last sector) and a
    # serial number from the label, for a reproducible image
    with open(path, "wb") as f:
        f.write(bytes(1474560))
    serial = hashlib.sha256(label.encode()).hexdigest()[:8]
    subprocess.run(["mformat", "-i", path, "-f", "1440", "-v", label, "-N", serial, "::"], check=True, env=env)
    dirs = sorted({"\\".join(f.split("\\")[:i]) for f in files for i in range(1, f.count("\\") + 1)},
                  key=lambda d: (d.count("\\"), d))
    for d in dirs:
        subprocess.run(["mmd", "-i", path, "::/" + d.replace("\\", "/")], check=True, env=env)
    for f in files:
        src = os.path.join(root, f.replace("\\", os.sep))
        os.utime(src, (epoch, epoch))
        subprocess.run(["mcopy", "-m", "-i", path, src, "::/" + f.replace("\\", "/")], check=True, env=env)


def pack(a):
    files = list_files(a.files)
    for f in files:
        if f.upper() in ("SETUP.EXE", "SETUP.INF"):
            sys.exit("%s: SETUP.EXE and SETUP.INF are added by w16pack" % a.files)
        for part in f.split("\\"):
            if not dos_name_ok(part):
                sys.exit("not a DOS 8.3 name (upper case): %s" % f)
    stage = os.path.join(a.out, a.name)
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(a.files, stage)
    shutil.copy(a.setup, os.path.join(stage, "SETUP.EXE"))
    write_inf(a.inf, files, a.version, os.path.join(stage, "SETUP.INF"))
    allf = ["SETUP.EXE", "SETUP.INF"] + files
    epoch = source_date(os.path.dirname(os.path.abspath(a.inf)))
    outputs = [os.path.join(a.out, a.name + ".zip")]
    make_zip(stage, allf, outputs[0], epoch)
    total = sum(os.path.getsize(os.path.join(stage, f.replace("\\", os.sep))) for f in allf)
    if not a.no_floppy:
        if total > 1400 * 1024:
            sys.exit("the files (%d bytes) do not fit on a 1.44 MB disk (use --no-floppy)" % total)
        outputs.append(os.path.join(a.out, a.name + "-disk.img"))
        label = (a.label or a.name).upper()[:11]
        make_floppy(stage, allf, outputs[1], label, epoch)
    with open(os.path.join(a.out, "SHA256SUMS"), "w") as f:
        for p in outputs:
            with open(p, "rb") as fh:
                f.write("%s  %s\n" % (hashlib.sha256(fh.read()).hexdigest(), os.path.basename(p)))
    for p in outputs + [os.path.join(a.out, "SHA256SUMS")]:
        print("%-50s %8d" % (p, os.path.getsize(p)))
    print("%d files, %d bytes" % (len(allf), total))


def preview(a):
    from PIL import Image
    im = Image.open(a.input).convert("RGB")
    im = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    im.save(a.output, "BMP")
    if os.path.getsize(a.output) > 65534:
        sys.exit("%s is larger than 64 KB: use a smaller image" % a.output)


def main():
    ap = argparse.ArgumentParser(description="Packages a Windows 3.1 product with Win16Setup.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--inf", required=True, help="SETUP.INF template")
    p.add_argument("--files", required=True, help="directory with the product's files")
    p.add_argument("--setup", required=True, help="SETUP.EXE")
    p.add_argument("--out", required=True)
    p.add_argument("--name", required=True, help="base name of the packages, e.g. product-1.0")
    p.add_argument("--label", help="volume label of the floppy image (default: name)")
    p.add_argument("--version", help="replaces @VERSION@ in the template")
    p.add_argument("--no-floppy", action="store_true")
    p.set_defaults(fn=pack)
    p = sub.add_parser("preview")
    p.add_argument("input")
    p.add_argument("output")
    p.set_defaults(fn=preview)
    a = ap.parse_args()
    os.makedirs(getattr(a, "out", "."), exist_ok=True)
    a.fn(a)


if __name__ == "__main__":
    main()
