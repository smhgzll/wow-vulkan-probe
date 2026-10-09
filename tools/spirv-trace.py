#!/usr/bin/python3
"""Trace where a SPIR-V shader's values come from (no spirv-tools needed): for every depth-compare sample (the game's shadow
maps) the image binding and the expression tree of its coordinate and reference, or for a vertex shader the expression of
every output at a location. Leaves are inputs (location), uniform members (set/binding + constant indices), push
constants, spec constants and literals; DXVK's private register variables are followed through their stores.

  tools/spirv-trace.py SHADER.spv [--depth N]                   # fragment: the depth-compare samples
  tools/spirv-trace.py SHADER.spv --outputs [LOC ...]            # vertex: the outputs (all, or these locations)
"""
import argparse, struct, sys

OPS = {12: "ExtInst", 57: "Call", 61: "Load", 65: "AccessChain", 66: "InBoundsAccessChain", 77: "VecExtractDyn", 79: "Shuffle",
       80: "Construct", 81: "Extract", 82: "Insert", 83: "Copy", 86: "SampledImage", 87: "Sample", 88: "SampleLod", 89: "SampleDref",
       90: "SampleDrefLod", 91: "SampleProj", 92: "SampleProjLod", 93: "SampleProjDref", 94: "SampleProjDrefLod", 95: "Fetch",
       109: "FToU", 110: "FToS", 111: "SToF", 112: "UToF", 124: "Bitcast", 126: "SNeg", 127: "FNeg", 128: "IAdd", 129: "FAdd",
       130: "ISub", 131: "FSub", 132: "IMul", 133: "FMul", 134: "UDiv", 135: "SDiv", 136: "FDiv", 141: "FMod",
       142: "VecTimesScalar", 143: "MatTimesScalar", 144: "VecTimesMat", 145: "MatTimesVec", 146: "MatTimesMat", 148: "Dot",
       164: "LogEq", 166: "LogOr", 167: "LogAnd", 168: "LogNot", 169: "Select", 170: "IEq", 171: "INe", 172: "UGt", 173: "SGt",
       174: "UGe", 175: "SGe", 176: "ULt", 177: "SLt", 180: "FOrdEq", 182: "FOrdNe", 184: "FOrdLt", 186: "FOrdGt",
       188: "FOrdLe", 190: "FOrdGe", 194: "ShrL", 195: "ShrA", 196: "Shl", 197: "Or", 198: "Xor", 199: "And",
       201: "BitInsert", 202: "BitSExtract", 203: "BitUExtract", 245: "Phi", 52: "SpecConstantOp"}
GLSL = {1: "round", 4: "fabs", 6: "fsign", 8: "floor", 9: "ceil", 10: "fract", 26: "pow", 27: "exp", 28: "log", 29: "exp2",
        30: "log2", 31: "sqrt", 32: "inversesqrt", 37: "fmin", 40: "fmax", 43: "fclamp", 46: "fmix", 48: "step", 49: "smoothstep",
        50: "fma", 66: "length", 68: "cross", 69: "normalize", 79: "nmin", 80: "nmax", 81: "nclamp"}
STORAGE = {0: "UniformConstant", 1: "Input", 2: "Uniform", 3: "Output", 6: "Private", 7: "Function", 9: "PushConstant", 12: "StorageBuffer"}


def parse(path):
    w = struct.unpack("<%dI" % (len(open(path, "rb").read()) // 4), open(path, "rb").read())
    if w[0] != 0x07230203: sys.exit("not SPIR-V")
    m = {"names": {}, "deco": {}, "types": {}, "consts": {}, "vars": {}, "ins": {}, "stores": {}, "exec": None, "order": []}
    i = 5
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        a = w[i + 1:i + n]
        if op == 5: m["names"][a[0]] = bytes(struct.pack("<%dI" % (n - 2), *a[1:])).split(b"\0")[0].decode(errors="replace")
        elif op == 15: m["exec"] = a[0]
        elif op == 71: m["deco"].setdefault(a[0], {})[a[1]] = list(a[2:])
        elif 19 <= op <= 39: m["types"][a[0]] = (op, list(a[1:]))
        elif op in (43, 50): m["consts"][a[1]] = (a[0], list(a[2:]), op == 50)
        elif op in (41, 42, 48, 49): m["consts"][a[1]] = (a[0], [int(op in (41, 48))], op >= 48)
        elif op in (44, 51): m["consts"][a[1]] = (a[0], ("composite", list(a[2:])), op == 51)
        elif op == 59: m["vars"][a[1]] = (a[0], a[2])
        elif op == 62:
            m["stores"].setdefault(a[0], []).append(a[1]); m["order"].append(("store", a[0], a[1]))
        elif op in OPS or op == 52:
            if op == 52: m["ins"][a[1]] = (op, a[0], [a[2]] + list(a[3:]))
            else: m["ins"][a[1]] = (op, a[0], list(a[2:]))
        i += n
    # Stores through access chains with constant indices (DXVK's register arrays: o[0] = ..., r[3] = ...): (var, indices) -> values
    m["elem"] = {}
    for ptr, values in m["stores"].items():
        ins = m["ins"].get(ptr)
        if ins and ins[0] in (65, 66) and ins[2][0] in m["vars"]:
            idx = tuple(m["consts"][k][1][0] if k in m["consts"] and not isinstance(m["consts"][k][1], tuple) else None for k in ins[2][1:])
            if None not in idx: m["elem"].setdefault((ins[2][0], idx), []).extend(values)
    return m


def float_of(m, cid):
    t, vals, spec = m["consts"][cid]
    top = m["types"].get(t, (0, []))
    if top[0] == 22 and vals and not isinstance(vals, tuple): return "%g" % struct.unpack("<f", struct.pack("<I", vals[0]))[0]
    if isinstance(vals, tuple): return "(" + ",".join(float_of(m, c) if c in m["consts"] else "%" + str(c) for c in vals[1]) + ")"
    return str(vals[0]) if vals else "?"


def describe_var(m, v):
    ptr, storage = m["vars"][v]
    d = m["deco"].get(v, {})
    name = m["names"].get(v, "")
    if 30 in d: where = "loc%d" % d[30][0]
    elif 11 in d: where = "builtin%d" % d[11][0]
    elif 33 in d: where = "set%d/b%d" % (d.get(34, [0])[0], d[33][0])
    else: where = "%" + str(v)
    return "%s:%s%s" % (STORAGE.get(storage, storage), where, ("(" + name + ")") if name else "")


def expr(m, i, depth, seen):
    if i in m["consts"]:
        t, vals, spec = m["consts"][i]
        s = float_of(m, i)
        if spec: s = "spec%s=%s" % (m["deco"].get(i, {}).get(1, ["?"])[0], s)
        return s
    if i in m["vars"]: return describe_var(m, i)
    if i not in m["ins"]: return "%" + str(i)
    if depth <= 0: return "%" + str(i) + "..."
    op, t, a = m["ins"][i]
    name = OPS.get(op, "op%d" % op)
    sub = lambda k: expr(m, k, depth - 1, seen)
    if op == 61:   # load: a variable directly, through an access chain, or a private register (its stores)
        p = a[0]
        pin = m["ins"].get(p)
        if pin and pin[0] in (65, 66) and pin[2][0] in m["vars"] and m["vars"][pin[2][0]][1] in (6, 7):
            idx = tuple(m["consts"][k][1][0] if k in m["consts"] and not isinstance(m["consts"][k][1], tuple) else None for k in pin[2][1:])
            st = m["elem"].get((pin[2][0], idx))
            if st and (p, depth) not in seen:
                seen.add((p, depth))
                return "%s%s{%s}" % (describe_var(m, pin[2][0]), list(idx), " | ".join(sub(v) for v in st[-2:]) + (" | +%d earlier" % (len(st) - 2) if len(st) > 2 else ""))
        if p in m["vars"]:
            storage = m["vars"][p][1]
            if storage in (6, 7) and p in m["stores"] and (p, depth) not in seen:
                seen.add((p, depth))
                st = m["stores"][p]
                return "%s{%s}" % (describe_var(m, p), " | ".join(sub(s) for s in st[-3:]) + (" | +%d earlier" % (len(st) - 3) if len(st) > 3 else ""))
            return describe_var(m, p)
        return sub(p)
    if op in (65, 66):
        base = a[0]
        return "%s[%s]" % (describe_var(m, base) if base in m["vars"] else sub(base), "][".join(sub(k) for k in a[1:]))
    if op == 12: return "%s(%s)" % (GLSL.get(a[1], "ext%d" % a[1]), ", ".join(sub(k) for k in a[2:]))
    if op == 79:   # shuffle: two vectors + component literals
        return "shuffle(%s, %s; %s)" % (sub(a[0]), sub(a[1]), ",".join(str(c) for c in a[2:]))
    if op == 81: return "%s.%s" % (sub(a[0]), ".".join(str(c) for c in a[1:]))
    if op == 82: return "insert(%s -> %s.%s)" % (sub(a[0]), sub(a[1]), ".".join(str(c) for c in a[2:]))
    if op in (202, 203): return "%s(%s, %s, %s)" % (name, sub(a[0]), sub(a[1]), sub(a[2]))
    if op == 52: return "specop%d(%s)" % (a[0], ", ".join(sub(k) for k in a[1:]))
    if op == 245: return "phi(%s)" % ", ".join(sub(a[k]) for k in range(0, len(a), 2))
    return "%s(%s)" % (name, ", ".join(sub(k) for k in a))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spv")
    ap.add_argument("--depth", type=int, default=9)
    ap.add_argument("--outputs", nargs="*", type=int)
    o = ap.parse_args()
    m = parse(o.spv)
    if o.outputs is not None:
        for v, (ptr, storage) in sorted(m["vars"].items()):
            if storage != 3: continue
            loc = m["deco"].get(v, {}).get(30, [None])[0]
            if o.outputs and loc not in o.outputs: continue
            print("OUTPUT %s:" % describe_var(m, v))
            stores = m["stores"].get(v, [])
            # an output written through access chains (per component): collect the chains' stores too
            chains = [k for k, (op, t, a) in m["ins"].items() if op in (65, 66) and a[0] == v]
            for c in chains: stores += [("%s[%s]" % ("", "][".join(expr(m, x, 2, set()) for x in m["ins"][c][2][1:])), s) for s in m["stores"].get(c, [])]
            for s in stores:
                if isinstance(s, tuple): print("  %s = %s" % (s[0], expr(m, s[1], o.depth, set())))
                else: print("  = %s" % expr(m, s, o.depth, set()))
        return
    found = 0
    for i, (op, t, a) in m["ins"].items():
        if op not in (89, 90, 93, 94): continue
        found += 1
        img = a[0]
        print("%s: image %s" % (OPS[op], expr(m, img, 3, set())))
        print("   coordinate = %s" % expr(m, a[1], o.depth, set()))
        print("   reference  = %s" % expr(m, a[2], o.depth, set()))
    if not found: print("no depth-compare samples")


if __name__ == "__main__":
    main()
