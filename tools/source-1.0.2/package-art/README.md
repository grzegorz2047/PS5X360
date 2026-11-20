# PS5X360Reloaded package art

`tools/source-1.0.2/package.py` puts these files into `PPSA83600/sce_sys/` of a release built
from source, in place of the official package's art, so the console shows PS5X360Reloaded as an
app of its own next to the official PS5X360 (PPSA50011). `package.py` checks each file against
the SHA-256 and the size below before using it, so a changed copy stops the packaging.

Each image is the official v1.0.2 package's own file (`PPSA50011.zip`, `sce_sys/`) with a red
"RELOADED" plate stamped on it (variant A3), at the official file's size and format (8-bit RGB
PNG).

| File | Package path | Size | SHA-256 |
|---|---|---|---|
| `icon0.png` | `sce_sys/icon0.png` | 512x512 | `0ea06e8927b1cf3a284a56e3440fd51017bc3e3efd1fc28b8015c4e6c662d50c` |
| `pic0.png` | `sce_sys/pic0.png` | 3840x2160 | `279db3b467fed42d9f5213c725a221e92ac919a8673ead61cb3b8755d7db1054` |
| `pic1.png` | `sce_sys/pic1.png` | 1920x1080 | `a44a30407639a3859a6efbffb9ec555a9a220f86c9b886e09e2e48bd8776e377` |
