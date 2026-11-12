"""Outils d'analyse de jetsetradio.exe (x86, PE32) : desassemblage, imports, references.
  python jsr.py imports [motif]          : adresses IAT des fonctions importees
  python jsr.py appels <nom_import>      : sites d'appel d'une fonction importee
  python jsr.py chaine <texte>           : adresse d'une chaine et instructions qui la referencent
  python jsr.py dis <adresse_hex> [n]    : desassemble n instructions
  python jsr.py fonction <adresse_hex>   : desassemble une fonction jusqu'au ret (limite 600)
  python jsr.py refs <adresse_hex>       : instructions qui referencent cette adresse (immediat/memoire/appel)"""
import os, sys, struct, pefile, capstone, functools
EXE = os.environ.get("JSR_EXE", "jetsetradio.exe")   # chemin de jetsetradio.exe (variable d'environnement JSR_EXE)
pe = pefile.PE(EXE, fast_load=False)
BASE = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
TEXT_VA = BASE + text.VirtualAddress; CODE = text.get_data()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = False
def read(va, n):
    return pe.get_data(va - BASE, n)
def imports():
    out = {}
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            out[(i.name or b"#%d" % i.ordinal).decode()] = (i.address, e.dll.decode())
    return out
@functools.lru_cache(None)
def all_insns():
    res = []; off = 0
    while off < len(CODE):
        got = False
        for ins in md.disasm(CODE[off:], TEXT_VA + off):
            res.append((ins.address, ins.size, ins.mnemonic, ins.op_str)); off = ins.address - TEXT_VA + ins.size; got = True
        if not got or off < len(CODE): off += 1
    return res
def refs(target):
    t = "0x%x" % target
    return [i for i in all_insns() if t in i[3]]
def dis(va, n):
    off = va - TEXT_VA
    for ins in md.disasm(CODE[off:off + n * 16], va):
        print("  %08X: %-7s %s" % (ins.address, ins.mnemonic, ins.op_str)); n -= 1
        if n <= 0: break
def fonction(va, limit=600):
    off = va - TEXT_VA
    for k, ins in enumerate(md.disasm(CODE[off:off + limit * 16], va)):
        print("  %08X: %-7s %s" % (ins.address, ins.mnemonic, ins.op_str))
        if ins.mnemonic == "ret" or k >= limit: break
if __name__ == "__main__":
    c = sys.argv[1]
    if c == "imports":
        m = sys.argv[2].lower() if len(sys.argv) > 2 else ""
        for n, (a, d) in sorted(imports().items()):
            if m in n.lower() or m in d.lower(): print("%08X %-30s %s" % (a, n, d))
    elif c == "appels":
        a = imports()[sys.argv[2]][0]
        for i in refs(a): print("  %08X: %s %s" % (i[0], i[2], i[3]))
    elif c == "chaine":
        s = sys.argv[2].encode(); data = open(EXE, "rb").read(); p = data.find(s)
        while p >= 0:
            va = pe.get_rva_from_offset(p) + BASE; print("chaine a %08X" % va)
            for i in refs(va): print("  %08X: %s %s" % (i[0], i[2], i[3]))
            p = data.find(s, p + 1)
    elif c == "dis": dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 40)
    elif c == "fonction": fonction(int(sys.argv[2], 16))
    elif c == "refs":
        for i in refs(int(sys.argv[2], 16)): print("  %08X: %s %s" % (i[0], i[2], i[3]))
