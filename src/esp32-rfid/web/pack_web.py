import gzip
from pathlib import Path

root = Path(__file__).parent

def write_gz(src, dst, array):
    data = (root / src).read_text(encoding="utf-8").encode("utf-8")
    gz = gzip.compress(data, compresslevel=9, mtime=0)

    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        f"const uint8_t {array}[] PROGMEM = {{"
    ]
    hexs = [f"0x{b:02X}" for b in gz]
    for i in range(0, len(hexs), 12):
        lines.append("  " + ", ".join(hexs[i:i+12]) + ",")
    lines.append("};")
    lines.append(f"const size_t {array}_len = sizeof({array});")
    lines.append("")
    output = "\n".join(lines).encode("ascii")
    if not (root / dst).exists() or (root / dst).read_bytes() != output:
        (root / dst).write_bytes(output)

write_gz("index.html", "index.html.gz.h", "index_html_gz")
write_gz("app.js", "app.js.gz.h", "app_js_gz")
write_gz("style.css", "style.css.gz.h", "style_css_gz")
write_gz("login.html", "login.html.gz.h", "login_html_gz")
