#!/usr/bin/env python3
"""Generate static HTML from 04.教程/*.md. Run from repo root or this directory."""

from __future__ import annotations

import html
import re
from pathlib import Path

import markdown
from markdown.extensions.tables import TableExtension

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parent
GITHUB_REPO_URL = "https://github.com/Zhentao-Lin/CX0001_ESP32_S31_Module_Lite_Board"
PINOUT_SRC = REPO_ROOT / "06.Picture" / "Pinout_CX0001.png"
PINOUT_ASSET = ROOT / "assets" / "Pinout_CX0001.png"

CHAPTERS: list[tuple[str, str]] = [
    ("00_开发环境.md", "00 · 开发环境"),
    ("01_GPIO_LED.md", "01 · GPIO LED"),
    ("02_GPIO_BUTTON.md", "02 · GPIO 按键"),
    ("03_WS2812.md", "03 · WS2812"),
    ("04_USB_SERIAL.md", "04 · USB 串口"),
    ("05_USB_MSC.md", "05 · USB 虚拟U盘"),
    ("06_BLE_UART.md", "06 · BLE 透传"),
    ("07_BLE_LED.md", "07 · BLE 控灯"),
    ("08_WIFI_SOCKET.md", "08 · Wi-Fi Socket"),
    ("09_WEB_WS2812.md", "09 · 网页控灯"),
]


def rewrite_links(text: str) -> str:
    """Point in-folder .md links at .html; keep repo paths as-is."""
    def sub_link(m: re.Match[str]) -> str:
        label, url = m.group(1), m.group(2)
        if url.startswith("http") or url.startswith("#"):
            return m.group(0)
        if url.endswith(".md"):
            url = url[:-3] + ".html"
        return f"{label}]({url})"

    return re.sub(r"([^\]]+)\]\(([^)]+)\)", sub_link, text)


def md_to_html_body(source: str) -> str:
    source = rewrite_links(source)
    return markdown.markdown(
        source,
        extensions=[TableExtension(), "fenced_code", "nl2br"],
        output_format="html5",
    )


def nav_html(prev_href: str | None, prev_label: str, next_href: str | None, next_label: str) -> str:
    prev = (
        f'<a class="nav-pill" href="{html.escape(prev_href)}">← {html.escape(prev_label)}</a>'
        if prev_href
        else '<span class="sep">—</span>'
    )
    nxt = (
        f'<a class="nav-pill" href="{html.escape(next_href)}">{html.escape(next_label)} →</a>'
        if next_href
        else '<span class="sep">—</span>'
    )
    return f"""<nav class="site-nav" aria-label="章节导航">
  <div class="site-nav-inner">
    <div class="nav-prev">{prev}</div>
    <div class="nav-center">
      <a class="nav-pill nav-pill-primary" href="index.html">教程目录</a>
      <a class="nav-pill" href="{GITHUB_REPO_URL}">仓库说明</a>
    </div>
    <div class="nav-next">{nxt}</div>
  </div>
</nav>"""


def html_name(md_fname: str) -> str:
    return Path(md_fname).stem + ".html"


def wrap_page(title: str, body: str, prev: tuple[str, str] | None, nxt: tuple[str, str] | None) -> str:
    prev_href, prev_label = (html_name(prev[0]), prev[1]) if prev else (None, "")
    next_href, next_label = (html_name(nxt[0]), nxt[1]) if nxt else (None, "")
    nav = nav_html(prev_href, prev_label, next_href, next_label)
    safe_title = html.escape(title)
    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{safe_title} — CX0001 教程</title>
  <meta name="theme-color" content="#171b26">
  <link rel="stylesheet" href="assets/style.css">
</head>
<body>
{nav}
<main class="content">
<article class="prose">
{body}
</article>
</main>
{nav}
<footer class="page-foot">CX0001 ESP32-S31 · ESP-IDF v6.1.1 教程（HTML 由 build_html.py 生成）</footer>
</body>
</html>
"""


def build_chapters() -> None:
    for i, (fname, short) in enumerate(CHAPTERS):
        md_path = ROOT / fname
        if not md_path.is_file():
            raise SystemExit(f"Missing: {md_path}")
        text = md_path.read_text(encoding="utf-8")
        body = md_to_html_body(text)
        prev = CHAPTERS[i - 1] if i > 0 else None
        nxt = CHAPTERS[i + 1] if i < len(CHAPTERS) - 1 else None
        out = ROOT / (fname.replace(".md", ".html"))
        title_match = re.search(r"^#\s+(.+)$", text, re.M)
        page_title = title_match.group(1).strip() if title_match else short
        out.write_text(wrap_page(page_title, body, prev, nxt), encoding="utf-8")
        print(f"Wrote {out.name}")


def build_index() -> None:
    rows = []
    for fname, short in CHAPTERS:
        href = html_name(fname)
        if fname.startswith("00"):
            sketch = "—"
        else:
            sketch_name = Path(fname).stem
            sketch = f'<a href="../03.Sketch/{html.escape(sketch_name)}">{html.escape(sketch_name)}</a>'
        rows.append(
            f"<tr><td>{html.escape(short)}</td>"
            f'<td><a href="{html.escape(href)}">阅读 HTML</a></td>'
            f"<td>{sketch}</td></tr>"
        )

    body = f"""<div class="index-hero">
<h1>CX0001 ESP32-S31 开发教程</h1>
<p>ESP-IDF <strong>v6.1.1</strong> · 目标芯片 <strong>esp32s31</strong>（<code>idf.py --preview</code>）</p>
</div>
<figure class="board-figure">
<img src="assets/Pinout_CX0001.png" alt="CX0001 板卡引脚图" width="960" loading="lazy">
<figcaption>引脚与外设对照 · 仓库原图见 <a href="{GITHUB_REPO_URL}/blob/main/06.Picture/Pinout_CX0001.png">Pinout_CX0001.png</a></figcaption>
</figure>
<h2>学习顺序</h2>
<table>
<thead><tr><th>章节</th><th>教程</th><th>示例工程</th></tr></thead>
<tbody>
{"".join(rows)}
</tbody>
</table>
<h2>板级引脚</h2>
<table>
<thead><tr><th>项目</th><th>值</th></tr></thead>
<tbody>
<tr><td>板载 LED</td><td>GPIO38（高有效）</td></tr>
<tr><td>BOOT 键</td><td>GPIO61（低有效）</td></tr>
<tr><td>WS2812</td><td>GPIO39（GRB）</td></tr>
<tr><td>烧录/调试</td><td>USB-Serial-JTAG</td></tr>
<tr><td>Flash / PSRAM</td><td>16MB / 16MB</td></tr>
</tbody>
</table>
<p>Markdown 源文件与 HTML 并存；修改 <code>.md</code> 后运行 <code>python build_html.py</code> 重新生成。</p>
<p>从零开始：<a href="00_开发环境.html">00 · 开发环境</a></p>"""

    prev_href, prev_label = None, ""
    next_href, next_label = html_name(CHAPTERS[0][0]), CHAPTERS[0][1]
    nav = nav_html(prev_href, prev_label, next_href, next_label)
    page = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>教程目录 — CX0001</title>
  <meta name="theme-color" content="#171b26">
  <link rel="stylesheet" href="assets/style.css">
</head>
<body>
{nav}
<main class="content">
<article class="prose">
{body}
</article>
</main>
{nav}
<footer class="page-foot">CX0001 ESP32-S31 · ESP-IDF v6.1.1 教程</footer>
</body>
</html>"""
    (ROOT / "index.html").write_text(page, encoding="utf-8")
    print("Wrote index.html")


def sync_assets() -> None:
    (ROOT / "assets").mkdir(exist_ok=True)
    if PINOUT_SRC.is_file():
        import shutil

        shutil.copy2(PINOUT_SRC, PINOUT_ASSET)


def main() -> None:
    sync_assets()
    build_chapters()
    build_index()


if __name__ == "__main__":
    main()