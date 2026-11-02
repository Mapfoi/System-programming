# -*- coding: utf-8 -*-
"""Собрать otchet5_DLL.docx: ключевые фрагменты кода с комментариями из исходников."""
import re
from pathlib import Path

from docx import Document
from docx.oxml.ns import qn
from docx.shared import Pt, Twips
from lxml import etree

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "otchet5_DLL.docx"

# Маркер файла, затем строки start..end (1-based, включительно) из path
FRAGMENTS = [
    [
        "// Лабораторная 5. DLL, статический/динамический импорт, CreateRemoteThread.",
        "",
    ],
    [
        "// --- memreplace_dll.c ---",
        (ROOT / "memreplace_dll.c", 9, 94),
    ],
    [
        "// --- static_import.c ---",
        (ROOT / "static_import.c", 11, 28),
    ],
    [
        "// --- dynamic_import.c ---",
        (ROOT / "dynamic_import.c", 14, 77),
    ],
    [
        "// --- inject_remote.c (адрес экспорта в мишени) ---",
        (ROOT / "inject_remote.c", 26, 65),
    ],
    [
        "// --- inject_remote.c (RemoteLoadDll) ---",
        (ROOT / "inject_remote.c", 67, 105),
    ],
    [
        "// --- inject_remote.c (main) ---",
        (ROOT / "inject_remote.c", 130, 169),
    ],
    [
        "// --- target_host.c ---",
        (ROOT / "target_host.c", 7, 24),
    ],
]

BOLD_RE = re.compile(
    r"\b(?:"
    r"VirtualQuery|VirtualProtect|VirtualAllocEx|VirtualFreeEx|"
    r"WriteProcessMemory|CreateRemoteThread|LoadLibraryW|GetProcAddress|FreeLibrary|"
    r"GetModuleFileNameW|GetModuleHandleW|OpenProcess|CloseHandle|"
    r"WaitForSingleObject|GetExitCodeThread|GetLastError|GetSystemInfo|GetCurrentProcessId|"
    r"CreateToolhelp32Snapshot|Module32FirstW|Module32NextW|"
    r"GetTickCount|Sleep|fflush|_setmode|_fileno|wprintf|wcslen|wcscat|wcsrchr|"
    r"strtoul|strncpy|memset|memcpy|memcmp|strlen|"
    r"DllMain|MemReplaceStringA|MemReplaceRemoteEntry|PageWritable|"
    r"RemoteModuleBase|RemoteProcAddress|RemoteLoadDll|WaitConsole|DllPathNextToExe|"
    r"__declspec|__cdecl|WINAPI|MEM_COMMIT|PAGE_READWRITE|PROCESS_CREATE_THREAD"
    r")\b"
)


def set_font(run, name, size, bold=False):
    run.bold = bold
    run.font.name = name
    run.font.size = Pt(size)
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = etree.SubElement(rpr, qn("w:rFonts"))
    for attr in ("w:ascii", "w:hAnsi", "w:eastAsia", "w:cs"):
        rfonts.set(qn(attr), name)


def add_fio_line(doc, text):
    p = doc.add_paragraph()
    run = p.add_run(text)
    set_font(run, "Times New Roman", 13, False)


def add_code_line(doc, line):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.space_before = Pt(0)
    pf.space_after = Pt(0)
    pf.line_spacing = 1.0
    text = line if line else " "
    pos = 0
    for match in BOLD_RE.finditer(text):
        if match.start() > pos:
            run = p.add_run(text[pos : match.start()])
            set_font(run, "Consolas", 8.5, False)
        run = p.add_run(match.group())
        set_font(run, "Consolas", 8.5, True)
        pos = match.end()
    if pos < len(text):
        run = p.add_run(text[pos:])
        set_font(run, "Consolas", 8.5, False)


def read_lines(path: Path, start: int, end: int):
    all_lines = path.read_text(encoding="utf-8").splitlines()
    if start < 1 or end > len(all_lines):
        raise ValueError(f"{path.name}: диапазон {start}-{end}, в файле {len(all_lines)} строк")
    return all_lines[start - 1 : end]


def collect_code_lines():
    lines = []
    for block in FRAGMENTS:
        for item in block:
            if isinstance(item, str):
                lines.append(item)
            else:
                path, start, end = item
                lines.extend(read_lines(path, start, end))
        lines.append("")
    return lines


def main():
    lines = collect_code_lines()
    doc = Document()
    sec = doc.sections[0]
    sec.page_width = Twips(11906)
    sec.page_height = Twips(16838)
    for margin in ("top_margin", "right_margin", "bottom_margin", "left_margin"):
        setattr(sec, margin, Twips(1440))
    sec.header_distance = Twips(720)
    sec.footer_distance = Twips(720)

    body = doc.element.body
    for child in list(body):
        if child.tag == qn("w:p"):
            body.remove(child)

    add_fio_line(doc, "ФИО группа")
    add_code_line(doc, "")

    for line in lines:
        add_code_line(doc, line)

    doc.save(OUT)
    print("saved", OUT, "code lines", len(lines))


if __name__ == "__main__":
    main()
