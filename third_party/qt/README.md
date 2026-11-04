# Qt's licence texts

`LICENSES/` is the `LICENSES/` directory of Qt's official qtbase 6.12.0 source
archive (`qtbase-everywhere-src-6.12.0.tar.xz`, SHA-256
`a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307`, from
https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/), unmodified.
Qt's binary releases do not carry them.

`extra/HPND-sell-variant.txt` is `src/gui/util/LICENSE.XSVG.txt` from the same
archive: Qt's own text for the XSVG code in Qt GUI (SBOM `Gui_Attribution_xsvg`,
licence HPND-sell-variant), which `LICENSES/` does not have. The macOS app bundles Qt, and
`packaging/macos/qt-notices.py` installs from here the texts of Qt and of the
code built into the Qt files that are bundled, as Qt's SBOM lists them.
