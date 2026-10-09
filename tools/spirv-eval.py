#!/usr/bin/python3
"""A small numeric SPIR-V interpreter for DXVK's D3D9 shaders (research: what exactly a game shader computes from the
constants a frame scan captured). Straight-line code, structured branches, switches, phis and function calls; no loops.

  import: m = Module(path); outputs, samples = m.run(inputs={loc: vec4}, buffers={(set, binding): bytes}, push=bytes,
                                                    spec={spec_id: value}, builtins={builtin_id: value})
  outputs: {("loc", n) or ("builtin", n): value}; samples: every image sample in order - ("dref"/"sample"/"fetch", set,
  binding, index, coordinate, reference). A depth-compare sample returns 1 (lit), a colour sample (.5, .5, .5, 1) - or
  run(texel=f): f(kind, set, binding, coordinate, reference) gives each sample's value (a synthetic shadow map; a "fetch"
  gets integer texel coordinates and returns a vec4). Image sizes: run(image_size=(w, h)), default 2048 x 2048.

CLI: tools/spirv-eval.py SHADER.spv   # prints the module's interface (inputs, outputs, buffers, spec constants)
"""
import math, struct, sys

NO_RESULT = {0, 255, 62, 249, 250, 251, 253, 254, 252, 247, 246, 5380, 8, 317, 63, 99, 224, 225, 56, 5, 6, 71, 72, 15, 16, 17, 11, 14, 3, 2, 7, 1, 4}
F32 = struct.Struct("<f")


def f32(x):
    try:
        return F32.unpack(F32.pack(x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def as_uint(x): return F32.unpack_from(F32.pack(x))[0] if False else struct.unpack("<I", struct.pack("<f", x))[0]
def as_float(u): return struct.unpack("<f", struct.pack("<I", u & 0xffffffff))[0]


class Kill(Exception):
    pass


class Module:
    def __init__(self, path):
        data = open(path, "rb").read()
        w = struct.unpack("<%dI" % (len(data) // 4), data)
        assert w[0] == 0x07230203
        self.names, self.deco, self.mdeco, self.types, self.consts, self.spec_ids = {}, {}, {}, {}, {}, {}
        self.vars, self.funcs, self.ext, self.entry = {}, {}, {}, None
        self.spec_default = {}
        i, func, block = 5, None, None
        while i < len(w):
            op, n = w[i] & 0xffff, w[i] >> 16
            a = list(w[i + 1:i + n])
            if op == 5: self.names[a[0]] = bytes(struct.pack("<%dI" % (n - 2), *a[1:])).split(b"\0")[0].decode(errors="replace")
            elif op == 11: self.ext[a[0]] = bytes(struct.pack("<%dI" % (n - 2), *a[1:])).split(b"\0")[0].decode()
            elif op == 15: self.entry = a[1]
            elif op == 71: self.deco.setdefault(a[0], {})[a[1]] = a[2:]
            elif op == 72: self.mdeco.setdefault(a[0], {}).setdefault(a[1], {})[a[2]] = a[3:]
            elif 19 <= op <= 39: self.types[a[0]] = (op, a[1:])
            elif op in (41, 42): self.consts[a[1]] = op == 41
            elif op in (48, 49): self.consts[a[1]] = op == 48; self.spec_default[a[1]] = op == 48
            elif op in (43, 50): self.consts[a[1]] = self.scalar(a[0], a[2]); (op == 50) and self.spec_default.__setitem__(a[1], self.consts[a[1]])
            elif op in (44, 51): self.consts[a[1]] = [self.consts[k] for k in a[2:]]
            elif op == 46: self.consts[a[1]] = self.zero(a[0])
            elif op == 52: self.consts[a[1]] = ("specop", a[0], a[2], a[3:])
            elif op == 59 and func is None: self.vars[a[1]] = (a[0], a[2], a[3] if len(a) > 3 else None)
            elif op == 54: func = {"id": a[1], "params": [], "blocks": {}, "order": []}; self.funcs[a[1]] = func
            elif op == 55: func["params"].append(a[1])
            elif op == 56: func = None
            elif op == 248: block = []; func["blocks"][a[0]] = block; func["order"].append(a[0])
            elif func is not None:
                if op in NO_RESULT or len(a) < 2: block.append((op, None, None, a))
                elif op == 59: block.append((op, a[0], a[1], a[2:]))
                else: block.append((op, a[0], a[1], a[2:]))
            i += n
        for k, v in list(self.deco.items()):
            if 1 in v: self.spec_ids[k] = v[1][0]

    # ---- types -------------------------------------------------------------------------------------------------------
    def scalar(self, t, word):
        op, a = self.types[t]
        if op == 22: return as_float(word)
        if op == 21: return word - (1 << 32) if a[1] and word & 0x80000000 else word
        return word

    def zero(self, t):
        op, a = self.types[t]
        if op == 20: return False
        if op in (21, 22): return 0.0 if op == 22 else 0
        if op in (23, 24): return [self.zero(a[0]) for _ in range(a[1])]
        if op == 28: return [self.zero(a[0]) for _ in range(self.consts[a[1]])]
        if op == 30: return [self.zero(m) for m in a]
        return None

    def size_of(self, t):
        op, a = self.types[t]
        if op in (20, 21, 22): return 4
        if op == 23: return 4 * a[1]
        if op == 24: return self.size_of(a[0]) * a[1]
        return 16

    def read_mem(self, t, data, off, stride=None):
        """A value of type t from a buffer (std140 per the module's Offset / ArrayStride decorations)."""
        op, a = self.types[t]
        if op == 22: return F32.unpack_from(data, off)[0] if off + 4 <= len(data) else 0.0
        if op == 21: return struct.unpack_from("<i" if a[1] else "<I", data, off)[0] if off + 4 <= len(data) else 0
        if op == 20: return (struct.unpack_from("<I", data, off)[0] if off + 4 <= len(data) else 0) != 0
        if op == 23: return [self.read_mem(a[0], data, off + 4 * k) for k in range(a[1])]
        if op in (28, 29):
            s = self.deco.get(t, {}).get(6, [16])[0]
            count = self.consts[a[1]] if op == 28 else max(0, (len(data) - off) // s)
            return [self.read_mem(a[0], data, off + s * k) for k in range(count)]
        if op == 30: return [self.read_mem(m, data, off + self.mdeco.get(t, {}).get(k, {}).get(35, [0])[0]) for k, m in enumerate(a)]
        return None

    def elem_offset(self, t, idx):
        op, a = self.types[t]
        if op == 30: return self.mdeco.get(t, {}).get(idx, {}).get(35, [0])[0], a[idx]
        if op in (28, 29): return self.deco.get(t, {}).get(6, [16])[0] * idx, a[0]
        if op == 23: return 4 * idx, a[0]
        if op == 24: return self.deco.get(t, {}).get(7, [16])[0] * idx, a[0]
        raise ValueError("offset into type op %d" % op)

    def pointee(self, ptr_t): return self.types[ptr_t][1][1]

    # ---- execution ---------------------------------------------------------------------------------------------------
    def run(self, inputs=None, buffers=None, push=b"", spec=None, builtins=None, images=None, dref_value=1.0, sample_value=None,
            texel=None, image_size=(2048, 2048)):
        """dref_value: what every depth-compare sample returns (1 lit, 0 shadowed); sample_value: a colour sample's (default grey);
        texel: f(kind, set, binding, coordinate, reference) -> each sample's value instead (see the module doc)."""
        self.dref_value, self.sample_value = dref_value, sample_value or [.5, .5, .5, 1.0]
        self.texel, self.image_size = texel, list(image_size)
        self.inputs, self.buffers, self.push, self.spec = inputs or {}, buffers or {}, push, spec or {}
        self.builtins, self.images = builtins or {}, images or {}
        self.mem, self.samples, self.values = {}, [], {}
        for v, (pt, storage, init) in self.vars.items():
            t = self.pointee(pt)
            d = self.deco.get(v, {})
            if storage == 1:   # Input
                if 30 in d:
                    val = self.inputs.get(d[30][0])
                    self.mem[v] = list(val) if isinstance(val, (list, tuple)) else (val if val is not None else self.zero(t))
                    if isinstance(self.mem[v], list) and self.types[t][0] == 23 and len(self.mem[v]) != self.types[t][1][1]:
                        n = self.types[t][1][1]; self.mem[v] = (list(self.mem[v]) + [0.0] * n)[:n]
                elif 11 in d: self.mem[v] = self.builtins.get(d[11][0], self.zero(t))
                else: self.mem[v] = self.zero(t)
            elif storage in (6, 3, 7): self.mem[v] = self.const_value(init) if init else self.zero(t)
        self.call(self.entry, [])
        out = {}
        for v, (pt, storage, _) in self.vars.items():
            if storage != 3: continue
            d = self.deco.get(v, {})
            key = ("loc", d[30][0]) if 30 in d else ("builtin", d[11][0]) if 11 in d else ("var", self.names.get(v, v))
            out[key] = self.mem[v]
        return out, self.samples

    def const_value(self, cid):
        if cid in self.spec_ids and self.spec_ids[cid] in self.spec:
            return self.spec[self.spec_ids[cid]]
        c = self.consts[cid]
        if isinstance(c, tuple) and c[0] == "specop": return self.specop(c)
        if isinstance(c, list): return [self.const_value(k) if isinstance(k, int) and k in self.consts else k for k in c] if False else [x for x in c]
        return c

    def specop(self, c):
        _, t, opcode, ops = c
        vals = [self.get(o) for o in ops]
        return self.alu(opcode, t, vals, raw=ops)

    def get(self, i):
        if i in self.values: return self.values[i]
        if i in self.consts: return self.const_value(i)
        if i in self.vars: return ("ptr", i, [])
        raise KeyError("id %d" % i)

    def call(self, fid, args):
        f = self.funcs[fid]
        saved = dict(zip(f["params"], args))
        self.values.update(saved)
        block, prev = f["order"][0], None
        while True:
            nxt = None
            for op, rt, rid, a in f["blocks"][block]:
                if op == 249: nxt = a[0]; break
                if op == 250: nxt = a[1] if self.get(a[0]) else a[2]; break
                if op == 251:
                    sel = self.get(a[0]); nxt = a[1]
                    for k in range(2, len(a), 2):
                        if a[k] == sel: nxt = a[k + 1]; break
                    break
                if op == 253: return None
                if op == 254: return self.get(a[0])
                if op in (252, 5380):
                    self.samples.append(("kill",)); raise Kill()
                if op in (247, 246, 8, 317, 0): continue
                if op == 255: raise RuntimeError("OpUnreachable reached")
                if op == 245:   # phi: the value from the block we came from
                    for k in range(0, len(a), 2):
                        if a[k + 1] == prev: self.values[rid] = self.get(a[k]); break
                    continue
                self.exec(op, rt, rid, a)
            prev, block = block, nxt

    # pointers: ("ptr", var, [index path])
    def load(self, p):
        _, v, path = p
        pt, storage, _ = self.vars[v]
        t = self.pointee(pt)
        if storage in (2, 12, 9):   # Uniform / StorageBuffer / PushConstant: read bytes
            if storage == 9: data = self.push
            else:
                d = self.deco.get(v, {})
                data = self.buffers.get((d.get(34, [0])[0], d.get(33, [0])[0]), b"")
            off = 0
            for idx in path: o, t = self.elem_offset(t, idx); off += o
            return self.read_mem(t, data, off)
        if storage == 0:   # UniformConstant: an image / sampler descriptor
            d = self.deco.get(v, {})
            return ("desc", d.get(34, [0])[0], d.get(33, [0])[0], tuple(path), self.names.get(v, ""))
        x = self.mem[v]
        for idx in path: x = x[idx]
        return list(x) if isinstance(x, list) else x

    def store(self, p, val):
        _, v, path = p
        if not path: self.mem[v] = list(val) if isinstance(val, list) else val; return
        x = self.mem[v]
        for idx in path[:-1]: x = x[idx]
        x[path[-1]] = list(val) if isinstance(val, list) else val

    def exec(self, op, rt, rid, a):
        g = self.get
        if op == 59: self.mem[rid] = self.zero(self.pointee(rt)); self.vars[rid] = (rt, 7, None); self.values[rid] = ("ptr", rid, []); return
        if op == 61: self.values[rid] = self.load(g(a[0])); return
        if op == 62: self.store(g(a[0]), g(a[1])); return
        if op in (65, 66):
            base = g(a[0]); idx = [int(g(k)) for k in a[1:]]
            self.values[rid] = ("ptr", base[1], base[2] + idx); return
        if op == 57:
            self.values[rid] = self.call(a[0], [g(k) for k in a[1:]]); return
        if op == 12: self.values[rid] = self.extinst(a[1], [g(k) for k in a[2:]]); return
        if op == 86: self.values[rid] = ("simg", g(a[0]), g(a[1])); return
        if op in (87, 88, 89, 90, 91, 92, 93, 94, 95, 96):
            img = g(a[0]); coord = g(a[1])
            d = img[1] if img[0] == "simg" else img
            dref = g(a[2]) if op in (89, 90, 93, 94) else None
            kind = "dref" if dref is not None else "fetch" if op == 95 else "sample"
            self.samples.append((kind, d[1], d[2], d[3], coord, dref))
            if self.texel: self.values[rid] = self.texel(kind, d[1], d[2], coord, dref); return
            self.values[rid] = self.dref_value if dref is not None else list(self.sample_value); return
        if op == 100: s = g(a[0]); self.values[rid] = s[1] if s[0] == "simg" else s; return   # OpImage: the image of a sampled image
        if op in (101, 103, 104, 106, 107):   # image queries
            self.values[rid] = list(self.image_size) if op in (103, 104) else 1; return
        self.values[rid] = self.alu(op, rt, [g(k) if (k in self.values or k in self.consts or k in self.vars) else k for k in a], raw=a)

    def alu(self, op, rt, v, raw):
        top, targ = self.types[rt]
        def cw(f, *xs):   # component-wise
            if isinstance(xs[0], list): return [cw(f, *[x[i] if isinstance(x, list) else x for x in xs]) for i in range(len(xs[0]))]
            return f(*xs)
        U = lambda x: int(x) & 0xffffffff
        S = lambda x: (int(x) & 0xffffffff) - (1 << 32) if int(x) & 0x80000000 else int(x)
        if op == 80: return [x for x in v]                       # CompositeConstruct
        if op == 81:                                               # CompositeExtract
            x = v[0]
            for k in raw[1:]: x = x[k]
            return list(x) if isinstance(x, list) else x
        if op == 82:                                               # CompositeInsert
            import copy
            x = copy.deepcopy(v[1]); y = x
            for k in raw[2:-1]: y = y[k]
            y[raw[-1]] = v[0]; return x
        if op == 79:                                               # VectorShuffle
            a, b = v[0], v[1]; both = list(a) + list(b)
            return [both[k] if k != 0xffffffff else 0.0 for k in raw[2:]]
        if op == 77: return v[0][int(v[1])]                        # VectorExtractDynamic
        if op == 78: x = list(v[0]); x[int(v[2])] = v[1]; return x
        if op == 83: return v[0]                                   # CopyObject
        if op == 169: return cw(lambda c, x, y: x if c else y, v[0], v[1], v[2]) if isinstance(v[0], list) else (v[1] if v[0] else v[2])
        if op == 124:                                              # Bitcast
            to_float = top == 22 or (top == 23 and self.types[targ[0]][0] == 22)
            return cw(lambda x: as_float(U(x)) if to_float else as_uint(x) if isinstance(x, float) else U(x), v[0])
        if op == 109: return cw(lambda x: U(int(x)) if math.isfinite(x) else 0, v[0])
        if op == 110: return cw(lambda x: int(x) if math.isfinite(x) else 0, v[0])
        if op in (111, 112): return cw(lambda x: float(S(x) if op == 111 else U(x)), v[0])
        if op == 113: return cw(lambda x: U(x), v[0])
        if op == 114: return cw(lambda x: S(x), v[0])
        if op == 115: return cw(lambda x: f32(x), v[0])
        if op == 127: return cw(lambda x: -x, v[0])
        if op == 126: return cw(lambda x: -S(x), v[0])
        ar = {129: lambda x, y: f32(x + y), 131: lambda x, y: f32(x - y), 133: lambda x, y: f32(x * y),
              136: lambda x, y: f32(x / y) if y != 0 else (math.copysign(math.inf, x) if x != 0 else math.nan),
              141: lambda x, y: f32(math.fmod(x, y)) if y else math.nan, 139: lambda x, y: f32(x - y * math.floor(x / y)) if y else math.nan,
              128: lambda x, y: U(x + y), 130: lambda x, y: U(x - y), 132: lambda x, y: U(x * y),
              134: lambda x, y: U(x) // U(y) if U(y) else 0, 135: lambda x, y: int(S(x) / S(y)) if S(y) else 0,
              137: lambda x, y: U(x) % U(y) if U(y) else 0,
              194: lambda x, y: U(x) >> (U(y) & 31), 195: lambda x, y: U(S(x) >> (U(y) & 31)), 196: lambda x, y: U(U(x) << (U(y) & 31)),
              197: lambda x, y: U(x) | U(y), 198: lambda x, y: U(x) ^ U(y), 199: lambda x, y: U(x) & U(y),
              164: lambda x, y: x == y, 165: lambda x, y: x != y, 166: lambda x, y: bool(x) or bool(y), 167: lambda x, y: bool(x) and bool(y),
              170: lambda x, y: U(x) == U(y), 171: lambda x, y: U(x) != U(y), 172: lambda x, y: U(x) > U(y), 173: lambda x, y: S(x) > S(y),
              174: lambda x, y: U(x) >= U(y), 175: lambda x, y: S(x) >= S(y), 176: lambda x, y: U(x) < U(y), 177: lambda x, y: S(x) < S(y),
              178: lambda x, y: U(x) <= U(y), 179: lambda x, y: S(x) <= S(y),
              180: lambda x, y: x == y, 181: lambda x, y: x == y or math.isnan(x) or math.isnan(y), 182: lambda x, y: x != y and not (math.isnan(x) or math.isnan(y)),
              183: lambda x, y: x != y, 184: lambda x, y: x < y, 185: lambda x, y: not (x >= y), 186: lambda x, y: x > y, 187: lambda x, y: not (x <= y),
              188: lambda x, y: x <= y, 189: lambda x, y: not (x > y), 190: lambda x, y: x >= y, 191: lambda x, y: not (x < y)}
        if op in ar: return cw(ar[op], v[0], v[1])
        if op == 168: return cw(lambda x: not x, v[0])
        if op == 200: return cw(lambda x: U(~U(x)), v[0])
        if op == 142: return [f32(x * v[1]) for x in v[0]]         # VectorTimesScalar
        if op == 148: return f32(sum(x * y for x, y in zip(v[0], v[1])))
        if op == 154: return any(v[0]);
        if op == 155: return all(v[0])
        if op == 156: return cw(lambda x: math.isnan(x), v[0])
        if op == 157: return cw(lambda x: math.isinf(x), v[0])
        if op == 202 or op == 203:                                 # BitFieldSExtract / UExtract
            def ex(b, o, c):
                o, c = U(o), U(c)
                if c == 0: return 0
                r = (U(b) >> o) & ((1 << c) - 1)
                if op == 202 and r & (1 << (c - 1)): r -= 1 << c
                return U(r)
            return cw(ex, v[0], v[1], v[2])
        if op == 201:                                              # BitFieldInsert
            def ins(b, s, o, c):
                o, c = U(o), U(c); m = ((1 << c) - 1) << o
                return U((U(b) & ~m) | ((U(s) << o) & m))
            return cw(ins, v[0], v[1], v[2], v[3])
        if op == 205: return cw(lambda x: bin(U(x)).count("1"), v[0])
        if op == 145:                                              # MatrixTimesVector (column-major)
            m, x = v[0], v[1]; return [f32(sum(m[c][r] * x[c] for c in range(len(m)))) for r in range(len(m[0]))]
        if op == 144:                                              # VectorTimesMatrix
            x, m = v[0], v[1]; return [f32(sum(x[r] * m[c][r] for r in range(len(x)))) for c in range(len(m))]
        raise NotImplementedError("opcode %d" % op)

    def extinst(self, n, v):
        cw = lambda f, *xs: [cw(f, *[x[i] if isinstance(x, list) else x for x in xs]) for i in range(len(xs[0]))] if isinstance(xs[0], list) else f(*xs)
        fmin = lambda x, y: y if math.isnan(x) else x if math.isnan(y) else min(x, y)
        fmax = lambda x, y: y if math.isnan(x) else x if math.isnan(y) else max(x, y)
        t = {1: lambda x: float(round(x)), 2: lambda x: float(math.floor(x + .5)), 3: lambda x: float(math.trunc(x)), 4: abs, 5: lambda x: abs(x),
             6: lambda x: math.copysign(1.0, x) if x else 0.0, 8: lambda x: float(math.floor(x)), 9: lambda x: float(math.ceil(x)),
             10: lambda x: x - math.floor(x), 13: math.sin, 14: math.cos, 15: math.tan,
             27: lambda x: math.exp(x), 28: lambda x: math.log(x) if x > 0 else -math.inf, 29: lambda x: 2.0 ** x,
             30: lambda x: math.log2(x) if x > 0 else -math.inf, 31: lambda x: math.sqrt(x) if x >= 0 else math.nan,
             32: lambda x: 1 / math.sqrt(x) if x > 0 else math.inf}
        if n in t: return cw(lambda x: f32(t[n](x)), v[0])
        if n == 26: return cw(lambda x, y: f32(x ** y) if x > 0 or (x == 0 and y > 0) else (1.0 if y == 0 else math.nan), v[0], v[1])
        if n in (37, 79): return cw(lambda x, y: fmin(x, y), v[0], v[1])
        if n in (40, 80): return cw(lambda x, y: fmax(x, y), v[0], v[1])
        if n in (38, 39): return cw(lambda x, y: min(x, y), v[0], v[1])
        if n in (41, 42): return cw(lambda x, y: max(x, y), v[0], v[1])
        if n in (43, 81): return cw(lambda x, a, b: fmin(fmax(x, a), b), v[0], v[1], v[2])
        if n in (44, 45): return cw(lambda x, a, b: min(max(x, a), b), v[0], v[1], v[2])
        if n == 46: return cw(lambda x, y, a: f32(x * (1 - a) + y * a), v[0], v[1], v[2])
        if n == 48: return cw(lambda e, x: 0.0 if x < e else 1.0, v[0], v[1])
        if n == 49:
            def ss(e0, e1, x):
                tt = min(max((x - e0) / (e1 - e0), 0.0), 1.0) if e1 != e0 else 0.0
                return f32(tt * tt * (3 - 2 * tt))
            return cw(ss, v[0], v[1], v[2])
        if n == 50: return cw(lambda a, b, c: f32(a * b + c), v[0], v[1], v[2])
        if n == 66: return f32(math.sqrt(sum(x * x for x in v[0])))
        if n == 67: return f32(math.sqrt(sum((x - y) ** 2 for x, y in zip(v[0], v[1]))))
        if n == 68: a, b = v[0], v[1]; return [f32(a[1] * b[2] - a[2] * b[1]), f32(a[2] * b[0] - a[0] * b[2]), f32(a[0] * b[1] - a[1] * b[0])]
        if n == 69:
            l = math.sqrt(sum(x * x for x in v[0])); return [f32(x / l) if l else math.nan for x in v[0]]
        raise NotImplementedError("GLSL.std.450 %d" % n)


def describe(path):
    m = Module(path)
    for v, (pt, storage, _) in sorted(m.vars.items()):
        d = m.deco.get(v, {})
        where = "loc%d" % d[30][0] if 30 in d else "builtin%d" % d[11][0] if 11 in d else "set%d/b%d" % (d.get(34, [0])[0], d[33][0]) if 33 in d else ""
        print("  %-14s storage %-2d %s" % (m.names.get(v, v), storage, where))
    print("  spec constants:", {m.names.get(k, k): (sid, m.spec_default.get(k)) for k, sid in m.spec_ids.items()})


if __name__ == "__main__":
    describe(sys.argv[1])
