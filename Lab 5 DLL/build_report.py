# -*- coding: utf-8 -*-
"""Собрать otchet5_DLL.docx: код лабораторной 5 с полужирными именами WinAPI."""
import re
from pathlib import Path

from docx import Document
from docx.oxml.ns import qn
from docx.shared import Pt, Twips
from lxml import etree

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "otchet5_DLL.docx"

SECTIONS = [
    ("memreplace_api.h", ROOT / "memreplace_api.h"),
    ("memreplace_dll.c — DLL, поиск и замена в виртуальной памяти", ROOT / "memreplace_dll.c"),
    ("static_import.c — статический импорт (__declspec(dllimport))", ROOT / "static_import.c"),
    ("dynamic_import.c — динамический импорт LoadLibraryW / GetProcAddress", ROOT / "dynamic_import.c"),
    ("inject_remote.c — внедрение DLL удалённым потоком", ROOT / "inject_remote.c"),
    ("target_host.c — процесс-мишень для демонстрации", ROOT / "target_host.c"),
]

INTRO = [
    "// Лабораторная работа 5. Динамически загружаемая библиотека (DLL).",
    "// DLL — модуль с кодом и данными, подключаемый к процессу при загрузке или по LoadLibrary.",
    "// Статический импорт: линковщик записывает зависимость от DLL, функции вызываются по IAT.",
    "// Динамический импорт: LoadLibraryW загружает DLL, GetProcAddress возвращает адрес экспорта.",
    "// Внедрение DLL: CreateRemoteThread в чужом процессе вызывает LoadLibraryW с путём к файлу.",
    "// После загрузки адрес экспорта в целевом процессе = база модуля + смещение от локальной загрузки.",
    "",
]

BOLD_RE = re.compile(
    r"\b(?:"
    r"VirtualQuery|VirtualProtect|VirtualAllocEx|VirtualFreeEx|"
    r"WriteProcessMemory|CreateRemoteThread|LoadLibraryW|GetProcAddress|FreeLibrary|"
    r"GetModuleFileNameW|GetModuleHandleW|OpenProcess|OpenProcessToken|"
    r"LookupPrivilegeValueW|AdjustTokenPrivileges|CloseHandle|"
    r"WaitForSingleObject|GetExitCodeThread|GetLastError|GetSystemInfo|GetCurrentProcessId|"
    r"CreateToolhelp32Snapshot|Module32FirstW|Module32NextW|FindRemoteModule|"
    r"GetTickCount|Sleep|fflush|_setmode|_fileno|wprintf|wcslen|wcscat|wcsrchr|"
    r"strtoul|strncpy|memset|memcpy|memcmp|strlen|"
    r"DllMain|MemReplaceStringA|MemReplaceRemoteEntry|"
    r"RegionWritable|ReplaceInBuffer|ScanCurrentProcess|"
    r"ResolveDllPath|InjectLibrary|ResolveRemoteExport|CallRemoteReplace|EnableDebugPrivilege|"
    r"PrintBanner"
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


def collect_lines():
    lines = list(INTRO)
    for title, path in SECTIONS:
        lines.append(f"// --- {title} ---")
        lines.extend(path.read_text(encoding="utf-8").splitlines())
        lines.append("")
    return lines


def main():
    lines = collect_lines()
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

    p = doc.add_paragraph()
    run = p.add_run("ФИО группа")
    set_font(run, "Times New Roman", 13, False)
    add_code_line(doc, "")

    for line in lines:
        add_code_line(doc, line)

    doc.save(OUT)
    print("saved", OUT, "lines", len(lines))


if __name__ == "__main__":
    main()
