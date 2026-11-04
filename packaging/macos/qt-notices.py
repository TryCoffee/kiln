#!/usr/bin/env python3
"""Qt's notices for the Qt files bundled into Kiln.app.

    qt-notices.py SBOM CONTENTS LICENSES OUT

SBOM      Qt's SPDX SBOM (<qt>/sbom/qtbase-<version>.spdx.json)
CONTENTS  the app's Contents/ folder, after macdeployqt
LICENSES  folders of licence texts named <SPDX id>.txt, separated by ':'
          (third_party/qt/LICENSES:third_party/qt/extra)
OUT       where NOTICES.md and the licence texts go (Contents/Resources/licenses/qt)

Starts from the Qt files actually in the bundle (Frameworks/Qt*.framework,
PlugIns/*/*.dylib), finds the SBOM package each one belongs to, and follows
DEPENDS_ON from there: the Qt modules and the third-party code built into
them (libpng, FreeType, HarfBuzz, PCRE2, ...). Writes one entry per package
with its licence and copyright, and copies the text of every licence named.
Fails when a bundled Qt file is not in the SBOM, or a licence has no text.
"""
import json
import os
import re
import shutil
import sys

QT_CHOICE = "LGPL-3.0-only"  # Kiln uses Qt under the LGPL v3
QT_TRIPLE = "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
OPERATORS = {"AND", "OR", "WITH"}


def bundled_qt_files(contents):
    """Paths as the SBOM writes them ("./lib/QtCore.framework/Versions/A/QtCore")."""
    found = {}
    fw = os.path.join(contents, "Frameworks")
    for name in sorted(os.listdir(fw)) if os.path.isdir(fw) else []:
        if name.startswith("Qt") and name.endswith(".framework"):
            mod = name[:-len(".framework")]
            found[f"./lib/{name}/Versions/A/{mod}"] = f"Frameworks/{name}"
    plugins = os.path.join(contents, "PlugIns")
    for root, _, files in os.walk(plugins):
        for f in sorted(files):
            if f.endswith(".dylib"):
                rel = os.path.relpath(os.path.join(root, f), plugins)
                found[f"./plugins/{rel}"] = f"PlugIns/{rel}"
    return found


def licence_ids(expression):
    return [t for t in re.findall(r"[A-Za-z0-9.+-]+", expression or "") if t not in OPERATORS]


def main(sbom_path, contents, licenses, out):
    sbom = json.load(open(sbom_path))
    packages = {p["SPDXID"]: p for p in sbom["packages"]}
    files = {f["SPDXID"]: f["fileName"] for f in sbom.get("files", [])}
    owner, depends = {}, {}
    for r in sbom["relationships"]:
        a, kind, b = r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]
        if kind == "CONTAINS" and b in files and a in packages:
            owner[files[b]] = a
        elif kind == "DEPENDS_ON" and a in packages and b in packages:
            depends.setdefault(a, []).append(b)

    bundled = bundled_qt_files(contents)
    if not bundled:
        sys.exit(f"no Qt files under {contents}")
    missing = [p for p in bundled if p not in owner]
    if missing:
        sys.exit("not in Qt's SBOM: " + ", ".join(bundled[p] for p in missing))

    seen, todo = set(), [owner[p] for p in bundled]
    while todo:
        pid = todo.pop()
        if pid not in seen:
            seen.add(pid)
            todo.extend(depends.get(pid, []))

    entries, texts = [], set()
    for pid in sorted(seen, key=lambda i: packages[i]["name"].lower()):
        p = packages[pid]
        lic = p.get("licenseConcluded") or p.get("licenseDeclared") or "NOASSERTION"
        if lic in ("NOASSERTION", "NONE"):
            continue  # Qt's own build-internal targets: covered by the module that contains them
        if lic == QT_TRIPLE:
            used = f"{QT_CHOICE} (Qt offers {lic})"
            texts.update([QT_CHOICE, "GPL-3.0-only"])  # the LGPL v3 is the GPL v3 plus its permissions
        else:
            used = lic
            texts.update(licence_ids(lic))
        name = p["name"].replace("_Attribution_", ": ")
        copyright = (p.get("copyrightText") or "").strip()
        entries.append((name, p.get("versionInfo") or "", used, copyright))

    os.makedirs(out, exist_ok=True)
    have = {}
    for folder in licenses.split(":"):
        for f in os.listdir(folder):
            if f.endswith(".txt"):
                have.setdefault(f[:-4], os.path.join(folder, f))
    lacking = sorted(t for t in texts if t not in have)
    if lacking:
        sys.exit("no licence text in " + licenses + " for: " + ", ".join(lacking))
    for t in sorted(texts):
        shutil.copyfile(have[t], os.path.join(out, t + ".txt"))

    # qtbase's versionInfo is a git ref ("025bdad1+HEAD"); the file name has the release.
    m = re.search(r"qtbase-(\d+\.\d+\.\d+)", os.path.basename(sbom_path))
    if not m:
        sys.exit(f"no Qt version in the SBOM's file name: {sbom_path}")
    version = m.group(1)
    with open(os.path.join(out, "NOTICES.md"), "w") as f:
        f.write(f"# Qt {version}\n\n"
                "Kiln.app contains these files of the Qt toolkit, from Qt's official build for\n"
                "macOS, changed only as macdeployqt and the packaging do: their library paths\n"
                "point into the app, only their arm64 code is kept (lipo -thin), and they are\n"
                f"signed with the app. Kiln uses Qt under the {QT_CHOICE}.\n\n")
        for p in bundled.values():
            f.write(f"- `{p}`\n")
        f.write("\nThe Qt source code for this version is at\n"
                f"https://download.qt.io/official_releases/qt/{'.'.join(version.split('.')[:2])}/{version}/submodules/\n"
                "(qtbase-everywhere-src-" + version + ".tar.xz). The Qt frameworks and plug-ins\n"
                "in Contents/Frameworks and Contents/PlugIns can be replaced with your own build\n"
                "of the same Qt version; re-sign the app afterwards (codesign --force --deep -s - Kiln.app).\n\n"
                "What those files contain, from Qt's SBOM, with each part's licence (texts in\n"
                "this folder):\n\n")
        for name, ver, used, copyright in entries:
            f.write(f"## {name}{' ' + ver if ver and ver != 'unknown' else ''}\n\nLicence: {used}\n")
            if copyright and copyright != "NOASSERTION":
                f.write("\n" + "\n".join("    " + line for line in copyright.splitlines()) + "\n")
            f.write("\n")
    print(f"qt-notices: Qt {version}, {len(bundled)} Qt files, {len(entries)} parts, {len(texts)} licence texts -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
