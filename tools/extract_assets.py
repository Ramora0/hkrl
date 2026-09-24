"""Read the game's serialized asset files (READ-ONLY) into analysis/assets/: every object of the ported scenes and
every prefab they can spawn, with each component's serialized fields, decoded PlayMakerFSMs, ObjectPool startup
pools and every Animator's controller and clips.  Format: analysis/README.md "assets/".

    python tools/extract_assets.py                       # every ported scene (sim/generated/GG_*)
    python tools/extract_assets.py GG_Soul_Master ...    # these scenes
    python tools/extract_assets.py --check               # + cross-check against analysis/fsm and the dumps

The player build strips MonoBehaviour type trees, so they are generated from the game's managed DLLs
(oracle_Data/Managed, vanilla Assembly-CSharp from Assembly-CSharp.dll.v) with UnityPy's TypeTreeGenerator, which
reads assembly metadata; nothing is executed.  Built-in classes use UnityPy's 2020.2.2f1 trees.

PlayMakerFSM actions are serialized as ActionData (PlayMaker ActionData.cs); FsmDecoder is a port of its
CreateAction / LoadActionField / Get* readers (analysis/decomp/PlayMaker/HutongGames.PlayMaker/ActionData.cs
:652-1700, FsmUtility.cs:357-546) into the analysis/fsm/<scene>.json schema (oracle/Dump/FsmDumper.cs).
"""
import argparse
import glob
import gzip
import json
import os
import re
import struct
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(os.environ.get("HKRL_GAME") or os.path.join(ROOT, "game"), "oracle_Data")
OUT = os.path.join(ROOT, "analysis", "assets")
UNITY = "2020.2.2f1"

# GameObject.m_Tag: built-in tag ids (UnityCsReference 2020.2 Runtime/Export/.. TagManager: 0..7), custom tags from
# 20000 in TagManager.tags order.
BUILTIN_TAGS = {0: "Untagged", 1: "Respawn", 2: "Finish", 3: "EditorOnly", 5: "MainCamera", 6: "Player", 7: "GameController"}

# ParamDataType (analysis/decomp/PlayMaker/HutongGames.PlayMaker/ParamDataType.cs), in declaration order.
PDT = ["Integer", "Boolean", "Float", "String", "Color", "ObjectReference", "LayerMask", "Enum", "Vector2", "Vector3",
       "Vector4", "Rect", "Array", "Character", "AnimationCurve", "FsmFloat", "FsmInt", "FsmBool", "FsmString",
       "FsmGameObject", "FsmOwnerDefault", "FunctionCall", "FsmAnimationCurve", "FsmEvent", "FsmObject", "FsmColor",
       "Unsupported", "GameObject", "FsmVector3", "LayoutOption", "FsmRect", "FsmEventTarget", "FsmMaterial",
       "FsmTexture", "Quaternion", "FsmQuaternion", "FsmProperty", "FsmVector2", "FsmTemplateControl", "FsmVar",
       "CustomClass", "FsmArray", "FsmEnum"]
# VariableType (HutongGames.PlayMaker/VariableType.cs) for FsmArray.type / FsmVar.type.
VARTYPE = ["Float", "Int", "Bool", "GameObject", "String", "Vector2", "Vector3", "Color", "Rect", "Material",
           "Texture", "Quaternion", "Object", "Array", "Enum"]
# FsmVariables serialized lists -> the dump's bucket names (FsmDumper.cs Variables).
VAR_LISTS = [("Float", "floatVariables"), ("Int", "intVariables"), ("Bool", "boolVariables"),
             ("String", "stringVariables"), ("Vector2", "vector2Variables"), ("Vector3", "vector3Variables"),
             ("Rect", "rectVariables"), ("Quaternion", "quaternionVariables"), ("Color", "colorVariables"),
             ("GameObject", "gameObjectVariables"), ("Array", "arrayVariables"), ("Enum", "enumVariables"),
             ("Object", "objectVariables"), ("Material", "materialVariables"), ("Texture", "textureVariables")]
LINKSTYLE = ["Default", "Bezier", "Circuit"]
OWNER_OPT = ["UseOwner", "SpecifyGameObject"]
EVENT_TARGET = ["Self", "GameObject", "GameObjectFSM", "FSMComponent", "BroadcastAll", "HostFSM", "ParentFSM",
                "SubFSMs"]


def load_generator():
    """TypeTreeGenerator over the managed DLLs, with UnityPy's type names fixed for serialized collections: the
    generator names a List<T>/T[] node after T ("string actionNames", "FsmFloat fsmFloatParams"), which UnityPy then
    reads as a single T; a node whose first child is its `Array` is a vector unless it is a real string."""
    from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator
    from UnityPy.helpers.TypeTreeNode import TypeTreeNode

    class Gen(TypeTreeGenerator):
        def get_nodes_up(self, assembly, fullname):
            key = (assembly, fullname)
            if key in self.cache:
                return self.cache[key]
            if not assembly.endswith(".dll"):
                assembly += ".dll"
            base = self.get_nodes(assembly, fullname)
            nodes = []
            for i, b in enumerate(base):
                t = b.m_Type
                if (i + 3 < len(base) and base[i + 1].m_Type == "Array" and base[i + 1].m_Level == b.m_Level + 1
                        and t != "vector" and not (t == "string" and base[i + 3].m_Type == "char")):
                    t = "vector"
                nodes.append(TypeTreeNode(b.m_Level, t, b.m_Name, 0, 0, m_MetaFlag=b.m_MetaFlag))
            node = TypeTreeNode.from_list(nodes)
            self.cache[key] = node
            return node

    man = os.path.join(GAME, "Managed")
    gen = Gen(UNITY)
    # Vanilla Assembly-CSharp: the installed one is the modding API's patched build.
    dlls = sorted(f for f in os.listdir(man) if f.endswith(".dll") and f != "Assembly-CSharp.dll"
                  and not f.startswith("MMHOOK") and f != "unityscenerepacker.dll")
    for f in dlls:
        with open(os.path.join(man, f), "rb") as fh:
            gen.load_dll(fh.read())
    with open(os.path.join(man, "Assembly-CSharp.dll.v"), "rb") as fh:
        gen.load_dll(fh.read())
    return gen


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def vec(d, keys="xyz"):
    return {k: d[k] for k in keys} if d is not None else None


class Assets:
    """One UnityPy environment over every .assets file plus the requested level files; objects are addressed by
    "<file>:<pathID>"."""

    def __init__(self, levels):
        import UnityPy
        files = ["globalgamemanagers", "globalgamemanagers.assets", "resources.assets"]
        files += sorted(os.path.basename(p) for p in glob.glob(os.path.join(GAME, "sharedassets*.assets")))
        files += list(levels)
        self.env = UnityPy.load(*[os.path.join(GAME, f) for f in files])
        self.env.typetree_generator = load_generator()
        self.files = {os.path.basename(k): v for k, v in self.env.files.items()}
        self.cache = {}
        self.errors = []
        tm = next(o for o in self.files["globalgamemanagers"].objects.values() if o.type.name == "TagManager")
        self.tags = tm.parse_as_dict()["tags"]
        self.scripts = {}

    # ---- object access
    def oid(self, fname, pid):
        return "%s:%d" % (fname, pid)

    def reader(self, oid):
        f, p = oid.rsplit(":", 1)
        sf = self.files.get(f)                  # None: a file outside the environment (unity default resources)
        return sf.objects.get(int(p)) if sf is not None else None

    def ptr(self, sf_name, pp):
        """PPtr dict or object -> oid in the global space, None for null."""
        fid, pid = (pp["m_FileID"], pp["m_PathID"]) if isinstance(pp, dict) else (pp.m_FileID, pp.m_PathID)
        if pid == 0:
            return None
        if fid == 0:
            return self.oid(sf_name, pid)
        ext = self.files[sf_name].externals[fid - 1].path
        base = os.path.basename(ext)
        if base not in self.files:
            return "%s:%d" % (base, pid)      # a file outside the environment (unity default resources)
        return self.oid(base, pid)

    def raw(self, oid):
        """parse_as_dict of an object (MonoBehaviours through the generator); cached."""
        if oid in self.cache:
            return self.cache[oid]
        r = self.reader(oid)
        if r is None:
            self.cache[oid] = None
            return None
        try:
            d = r.parse_as_dict()
        except Exception as e:             # recorded, never guessed
            self.errors.append("%s %s: %s" % (oid, r.type.name, e))
            try:
                d = r.parse_as_dict(check_read=False)
                d["__partial"] = str(e)
            except Exception as e2:
                d = {"__error": str(e2)}
        self.cache[oid] = d
        return d

    def type_name(self, oid):
        r = self.reader(oid)
        return None if r is None else r.type.name

    def script_of(self, oid):
        """(namespace.class, assembly) of a MonoBehaviour."""
        if oid in self.scripts:
            return self.scripts[oid]
        head = self.reader(oid).parse_monobehaviour_head()      # the MonoBehaviour header alone (no generated tree)
        sid = self.ptr(oid.rsplit(":", 1)[0], head.m_Script)
        res = (None, None)
        if sid:
            s = self.raw(sid)
            if s:
                ns = s.get("m_Namespace") or ""
                res = ((ns + "." if ns else "") + s["m_ClassName"], s.get("m_AssemblyName"))
        self.scripts[oid] = res
        return res

    def tag(self, t):
        return BUILTIN_TAGS.get(t) or (self.tags[t - 20000] if t >= 20000 and t - 20000 < len(self.tags) else "#%d" % t)

    # ---- GameObject trees
    def transform_of(self, go_oid):
        fname = go_oid.rsplit(":", 1)[0]
        for c in self.raw(go_oid)["m_Component"]:
            cid = self.ptr(fname, c["component"])
            if self.type_name(cid) in ("Transform", "RectTransform"):
                return cid
        return None

    def root_of(self, go_oid):
        t = self.transform_of(go_oid)
        while True:
            td = self.raw(t)
            f = self.ptr(t.rsplit(":", 1)[0], td["m_Father"])
            if f is None:
                return self.ptr(t.rsplit(":", 1)[0], td["m_GameObject"])
            t = f

    def path_of(self, go_oid):
        parts = []
        t = self.transform_of(go_oid)
        while t:
            td = self.raw(t)
            g = self.ptr(t.rsplit(":", 1)[0], td["m_GameObject"])
            parts.append(self.raw(g)["m_Name"])
            t = self.ptr(t.rsplit(":", 1)[0], td["m_Father"])
        return "/".join(reversed(parts))

    def describe(self, oid):
        """A reference to any object: its type, name and, for scene objects/components, its GameObject path."""
        if oid is None:
            return None
        r = self.reader(oid)
        if r is None:
            return {"$ref": oid, "type": None}
        t = r.type.name
        out = {"$ref": oid, "type": t}
        d = self.raw(oid)
        if d is None:
            return out
        if t == "GameObject":
            out["name"] = d.get("m_Name")
            out["path"] = self.path_of(oid)
        elif "m_GameObject" in d:
            g = self.ptr(oid.rsplit(":", 1)[0], d["m_GameObject"])
            if g:
                out["name"] = self.raw(g).get("m_Name")
                out["path"] = self.path_of(g)
            else:                                       # a ScriptableObject (FsmTemplate, tk2d data assets)
                out["name"] = d.get("m_Name")
            if t == "MonoBehaviour":
                out["script"] = self.script_of(oid)[0]
        else:
            out["name"] = d.get("m_Name")
        return out

    def resolve(self, fname, v):
        """Serialized value with every PPtr replaced by a reference record."""
        if isinstance(v, dict):
            if set(v.keys()) == {"m_FileID", "m_PathID"}:
                return self.describe(self.ptr(fname, v))
            return {k: self.resolve(fname, x) for k, x in v.items()}
        if isinstance(v, list):
            return [self.resolve(fname, x) for x in v]
        if isinstance(v, (bytes, bytearray)):
            return list(v)
        return v

    def tree(self, root_go):
        """Every GameObject of the subtree at root_go in depth-first sibling order (Transform.m_Children)."""
        out = []

        def walk(go, parent, path):
            fname = go.rsplit(":", 1)[0]
            g = self.raw(go)
            name = g["m_Name"]
            p = name if not path else path + "/" + name
            t = self.transform_of(go)
            td = self.raw(t)
            rec = {"id": go, "path": p, "name": name, "parent": parent, "active": bool(g["m_IsActive"]),
                   "layer": g["m_Layer"], "tag": self.tag(g["m_Tag"]),
                   "localPosition": vec(td["m_LocalPosition"]), "localRotation": vec(td["m_LocalRotation"], "xyzw"),
                   "localScale": vec(td["m_LocalScale"]), "children": [], "components": []}
            for c in g["m_Component"]:
                cid = self.ptr(fname, c["component"])
                tn = self.type_name(cid)
                crec = {"id": cid, "class": tn}
                d = self.raw(cid)
                if tn == "MonoBehaviour":
                    crec["script"], crec["assembly"] = self.script_of(cid)
                if tn not in ("Transform", "RectTransform"):
                    data = {k: v for k, v in (d or {}).items() if k not in ("m_GameObject", "m_Script", "m_ObjectHideFlags",
                                                                             "m_CorrespondingSourceObject", "m_PrefabInstance",
                                                                             "m_PrefabAsset")}
                    crec["data"] = self.resolve(cid.rsplit(":", 1)[0], data)
                rec["components"].append(crec)
            out.append(rec)
            for ch in td["m_Children"]:
                ct = self.ptr(fname, ch)
                cg = self.ptr(ct.rsplit(":", 1)[0], self.raw(ct)["m_GameObject"])
                rec["children"].append(cg)
                walk(cg, go, p)

        walk(root_go, None, "")
        return out


# ------------------------------------------------------------------------------------------------ FSM decoding

class ActionTypes:
    """Declared C# types of action fields, which ActionData does not store (LoadActionField reads them by
    reflection).  Taken from the runtime dumps (FsmDumper ActionFields: `type` is FieldInfo.FieldType.FullName) of
    every dumped scene; a field no dump has falls back to its ParamDataType."""

    def __init__(self):
        self.t = {}
        paths = glob.glob(os.path.join(ROOT, "analysis", "fsm", "*.json"))
        paths += glob.glob(os.path.join(ROOT, "analysis", "dumps_all", "*", "fsm.json"))
        for p in paths:
            try:
                d = json.load(open(p, encoding="utf-8"))
            except Exception:
                continue
            for f in d.get("fsms", []):
                for s in f.get("states") or []:
                    for a in s.get("actions") or []:
                        m = self.t.setdefault(a.get("type"), {})
                        for fl in a.get("fields") or []:
                            m.setdefault(fl["name"], fl.get("type"))

    def get(self, action, field):
        return (self.t.get(action) or {}).get(field)


class FsmDecoder:
    """ActionData -> analysis/fsm schema.  `refs(fname, pptr)` describes an object reference."""

    def __init__(self, assets, types):
        self.A, self.T = assets, types
        self.unknown_types = set()

    # -- FsmVariables (FsmVariables.cs; NamedVariable name/useVariable/value)
    def named(self, kind, v, fname):
        if v is None:
            return None
        o = {"__fsm": kind, "name": v.get("name") or None, "useVariable": bool(v.get("useVariable"))}
        val = v.get("value")
        if kind in ("FsmFloat",):
            o["value"] = f32(val)
        elif kind in ("FsmInt",):
            o["value"] = int(val)
        elif kind == "FsmBool":
            o["value"] = bool(val)
        elif kind == "FsmString":
            o["value"] = val
        elif kind in ("FsmVector2",):
            o["value"] = vec(val, "xy")
        elif kind == "FsmVector3":
            o["value"] = vec(val)
        elif kind == "FsmQuaternion":
            o["value"] = vec(val, "xyzw")
        elif kind == "FsmRect":
            o["value"] = {"x": val["x"], "y": val["y"], "width": val["width"], "height": val["height"]}
        elif kind == "FsmColor":
            o["value"] = {"r": val["r"], "g": val["g"], "b": val["b"], "a": val["a"]}
        elif kind == "FsmGameObject":
            o["value"] = self.A.describe(self.A.ptr(fname, val))
        elif kind in ("FsmObject", "FsmMaterial", "FsmTexture"):
            o["type"] = v.get("typeName")
            o["value"] = self.A.describe(self.A.ptr(fname, val))
        elif kind == "FsmEnum":
            # FsmEnum.InitEnumType (FsmEnum.cs:108-113): a name that resolves to no type is HutongGames.PlayMaker.None
            en = v.get("enumName") or "HutongGames.PlayMaker.None"
            o["enumType"] = en
            o["value"] = {"__enum": en, "value": int(v.get("intValue", 0))}
        elif kind == "FsmArray":
            et = VARTYPE[v["type"]] if 0 <= v.get("type", -1) < len(VARTYPE) else "Unknown"
            o["elementType"] = et
            o["objectTypeName"] = v.get("objectTypeName")
            o["values"] = self.array_values(et, v, fname)
        return o

    def array_values(self, et, v, fname):
        """FsmArray.Values from its typed backing lists (FsmArray.cs: floatValues/intValues/... by VariableType)."""
        if et == "Float":
            return [f32(x) for x in v.get("floatValues") or []]
        if et in ("Int", "Enum"):
            return list(v.get("intValues") or [])
        if et == "Bool":
            return [bool(x) for x in v.get("boolValues") or []]
        if et == "String":
            return list(v.get("stringValues") or [])
        if et in ("Vector2", "Vector3", "Quaternion", "Rect", "Color"):
            keys = {"Vector2": "xy", "Vector3": "xyz", "Quaternion": "xyzw"}.get(et)
            out = []
            for q in v.get("vector4Values") or []:
                if et == "Rect":
                    out.append({"x": q["x"], "y": q["y"], "width": q["z"], "height": q["w"]})
                elif et == "Color":
                    out.append({"r": q["x"], "g": q["y"], "b": q["z"], "a": q["w"]})
                else:
                    out.append({k: q[k] for k in keys})
            return out
        if et in ("GameObject", "Object", "Material", "Texture"):
            return [self.A.describe(self.A.ptr(fname, p)) for p in v.get("objectReferences") or []]
        return []

    def variables(self, fv, fname):
        out = {}
        for bucket, key in VAR_LISTS:
            kind = "Fsm" + bucket
            out[bucket] = [self.named(kind, v, fname) for v in fv.get(key) or []]
        return out

    # -- ActionData (ActionData.cs)
    def decode_state(self, st, fsm, fname, var_index):
        ad = st["actionData"]
        names = ad.get("actionNames") or []
        byte = bytes(ad.get("byteData") or [])
        dv = fsm.get("dataVersion", 1)
        strp = ad.get("stringParams") or []
        pdt, ppos, psize, pname = ad["paramDataType"], ad["paramDataPos"], ad.get("paramByteDataSize") or [], ad.get("paramName") or []
        starts = list(ad.get("actionStartIndex") or [])
        actions = []

        def s_utf8(pos, n):
            return byte[pos:pos + n].decode("utf-8") if n else ""

        def var_ref(kind, name):
            """fsm.GetFsmX(name): the FSM's variable of that name (FsmVariables.GetFsmX), as the dump shows it."""
            v = var_index.get((kind, name))
            if v is None:
                return {"__fsm": kind, "name": name, "useVariable": True, "value": None, "__dangling": True}
            o = dict(v)
            o["useVariable"] = True
            return o

        def fsm_bytes(kind, i):
            """DataVersion 1 FsmX from byteData (FsmUtility.cs:385-519): value, useVariable, then the name."""
            pos, n = ppos[i], psize[i]
            head = {"FsmFloat": 5, "FsmInt": 5, "FsmBool": 2, "FsmVector2": 9, "FsmVector3": 13, "FsmRect": 17,
                    "FsmQuaternion": 17, "FsmColor": 17}[kind]
            nm = s_utf8(pos + head, n - head)
            if nm:
                return var_ref(kind, nm)
            o = {"__fsm": kind, "name": None}
            if kind == "FsmFloat":
                o["useVariable"] = bool(byte[pos + 4]); o["value"] = struct.unpack_from("<f", byte, pos)[0]
            elif kind == "FsmInt":
                o["useVariable"] = bool(byte[pos + 4]); o["value"] = struct.unpack_from("<i", byte, pos)[0]
            elif kind == "FsmBool":
                o["useVariable"] = bool(byte[pos + 1]); o["value"] = bool(byte[pos])
            elif kind == "FsmVector2":
                x, y = struct.unpack_from("<2f", byte, pos); o["useVariable"] = bool(byte[pos + 8]); o["value"] = {"x": x, "y": y}
            elif kind == "FsmVector3":
                x, y, z = struct.unpack_from("<3f", byte, pos); o["useVariable"] = bool(byte[pos + 12]); o["value"] = {"x": x, "y": y, "z": z}
            else:
                a, b, c, d = struct.unpack_from("<4f", byte, pos); o["useVariable"] = bool(byte[pos + 16])
                o["value"] = ({"x": a, "y": b, "width": c, "height": d} if kind == "FsmRect" else
                              {"r": a, "g": b, "b": c, "a": d} if kind == "FsmColor" else {"x": a, "y": b, "z": c, "w": d})
            return o

        def fsm_param(kind, lst, i):
            """DataVersion 2 FsmX from its typed list (ActionData.cs:1181-1375): a named one is the FSM's variable."""
            v = (ad.get(lst) or [None] * (ppos[i] + 1))[ppos[i]]
            if v is None:
                return {"__fsm": kind, "name": None, "useVariable": False, "value": None}
            if v.get("name"):
                return var_ref(kind, v["name"])
            return self.named(kind, v, fname)

        def get(kind, i):
            if kind in ("FsmFloat", "FsmInt", "FsmBool", "FsmVector2", "FsmVector3", "FsmRect", "FsmQuaternion", "FsmColor"):
                if dv > 1:
                    return fsm_param(kind, {"FsmFloat": "fsmFloatParams", "FsmInt": "fsmIntParams", "FsmBool": "fsmBoolParams",
                                            "FsmVector2": "fsmVector2Params", "FsmVector3": "fsmVector3Params",
                                            "FsmRect": "fsmRectParams", "FsmQuaternion": "fsmQuaternionParams",
                                            "FsmColor": "fsmColorParams"}[kind], i)
                return fsm_bytes(kind, i)
            if kind == "FsmGameObject":
                return fsm_param(kind, "fsmGameObjectParams", i)
            if kind == "FsmString":
                return fsm_param(kind, "fsmStringParams", i)
            if kind in ("FsmObject", "FsmMaterial", "FsmTexture"):
                v = (ad.get("fsmObjectParams") or [])[ppos[i]]
                if v and v.get("name"):
                    return var_ref(kind, v["name"])
                o = self.named("FsmObject", v, fname) if v else {"__fsm": kind, "name": None, "useVariable": False, "value": None}
                o["__fsm"] = kind
                return o
            if kind == "FsmEnum":
                return fsm_param(kind, "fsmEnumParams", i)
            if kind == "FsmArray":
                return fsm_param(kind, "fsmArrayParams", i)
            if kind == "FsmOwnerDefault":
                v = (ad.get("fsmOwnerDefaultParams") or [])[ppos[i]]
                go = v["gameObject"]
                g = var_ref("FsmGameObject", go["name"]) if (v["ownerOption"] != 0 and go.get("name")) else self.named("FsmGameObject", go, fname)
                return {"__fsm": "FsmOwnerDefault", "ownerOption": OWNER_OPT[v["ownerOption"]], "gameObject": g}
            if kind == "FsmEvent":
                nm = strp[ppos[i]] if (dv > 1 and strp) else s_utf8(ppos[i], psize[i])
                return {"__fsm": "FsmEvent", "name": nm, "isGlobal": nm in self.global_events} if nm else None
            raise KeyError(kind)

        def unser(t, fields):
            return {"__type": t, "__unserialized": True, "__reason": "unhandled-type", "__fields": fields}

        def value(i, atype, fname_):
            """One parameter.  Returns (declared type, value, params consumed after i)."""
            k = PDT[pdt[i]] if 0 <= pdt[i] < len(PDT) else "?%d" % pdt[i]
            decl = self.T.get(atype, fname_)
            pos = ppos[i]
            if k in ("FsmFloat", "FsmInt", "FsmBool", "FsmVector2", "FsmVector3", "FsmRect", "FsmQuaternion", "FsmColor",
                     "FsmGameObject", "FsmString", "FsmObject", "FsmMaterial", "FsmTexture", "FsmEnum", "FsmArray",
                     "FsmOwnerDefault", "FsmEvent"):
                return decl or "HutongGames.PlayMaker." + k, get(k, i), 0
            if k == "Integer":
                return decl or "System.Int32", struct.unpack_from("<i", byte, pos)[0], 0
            if k == "Float":
                return decl or "System.Single", struct.unpack_from("<f", byte, pos)[0], 0
            if k == "Boolean":
                return decl or "System.Boolean", bool(byte[pos]), 0
            if k == "String":
                return decl or "System.String", (strp[pos] if (dv > 1 and strp) else s_utf8(pos, psize[i])), 0
            if k == "Enum":
                return decl or "?enum", {"__enum": decl, "value": struct.unpack_from("<i", byte, pos)[0]}, 0
            if k == "LayerMask":
                return decl or "UnityEngine.LayerMask", {"__layerMask": struct.unpack_from("<i", byte, pos)[0]}, 0
            if k in ("Vector2", "Vector3", "Vector4", "Color", "Rect", "Quaternion"):
                n = {"Vector2": 2, "Vector3": 3}.get(k, 4)
                xs = struct.unpack_from("<%df" % n, byte, pos)
                keys = {"Vector2": "xy", "Vector3": "xyz", "Vector4": "xyzw", "Quaternion": "xyzw"}.get(k)
                if k == "Color":
                    return decl or "UnityEngine.Color", dict(zip(("r", "g", "b", "a"), xs)), 0
                if k == "Rect":
                    return decl or "UnityEngine.Rect", dict(zip(("x", "y", "width", "height"), xs)), 0
                return decl or "UnityEngine." + k, dict(zip(keys, xs)), 0
            if k in ("ObjectReference", "GameObject"):
                ref = (ad.get("unityObjectParams") or [])[pos]
                return decl or "UnityEngine.Object", self.A.describe(self.A.ptr(fname, ref)), 0
            if k == "FsmVar":
                v = (ad.get("fsmVarParams") or [])[pos]
                return decl or "HutongGames.PlayMaker.FsmVar", unser("HutongGames.PlayMaker.FsmVar", {
                    "variableName": v.get("variableName"), "objectType": v.get("objectType"),
                    "useVariable": bool(v.get("useVariable")),
                    "type": {"__enum": "HutongGames.PlayMaker.VariableType", "value": v.get("type")},
                    "floatValue": v.get("floatValue"), "intValue": v.get("intValue"), "boolValue": bool(v.get("boolValue")),
                    "stringValue": v.get("stringValue"), "vector4Value": v.get("vector4Value")}), 0
            if k == "FsmEventTarget":
                v = (ad.get("fsmEventTargetParams") or [])[pos]
                od = v["gameObject"]; go = od["gameObject"]
                g = var_ref("FsmGameObject", go["name"]) if go.get("name") else self.named("FsmGameObject", go, fname)
                ex = v["excludeSelf"]; fn = v["fsmName"]; sc = v["sendToChildren"]
                return decl or "HutongGames.PlayMaker.FsmEventTarget", unser("HutongGames.PlayMaker.FsmEventTarget", {
                    "target": {"__enum": "HutongGames.PlayMaker.FsmEventTarget+EventTarget", "value": v["target"],
                               "name": EVENT_TARGET[v["target"]] if 0 <= v["target"] < len(EVENT_TARGET) else None},
                    "excludeSelf": var_ref("FsmBool", ex["name"]) if ex.get("name") else self.named("FsmBool", ex, fname),
                    "gameObject": {"__fsm": "FsmOwnerDefault", "ownerOption": OWNER_OPT[od["ownerOption"]], "gameObject": g},
                    "fsmName": var_ref("FsmString", fn["name"]) if fn.get("name") else self.named("FsmString", fn, fname),
                    "sendToChildren": var_ref("FsmBool", sc["name"]) if sc.get("name") else self.named("FsmBool", sc, fname),
                    "fsmComponent": self.A.describe(self.A.ptr(fname, v.get("fsmComponent")))}), 0
            if k == "FunctionCall":
                v = (ad.get("functionCallParams") or [])[pos]
                fl = {"FunctionName": v.get("FunctionName"), "parameterType": v.get("parameterType")}
                for pk, kind in (("BoolParameter", "FsmBool"), ("FloatParameter", "FsmFloat"), ("IntParameter", "FsmInt"),
                                 ("GameObjectParameter", "FsmGameObject"), ("ObjectParameter", "FsmObject"),
                                 ("StringParameter", "FsmString"), ("Vector2Parameter", "FsmVector2"),
                                 ("Vector3Parameter", "FsmVector3"), ("RectParamater", "FsmRect"), ("ColorParameter", "FsmColor"),
                                 ("MaterialParameter", "FsmMaterial"), ("TextureParameter", "FsmTexture"),
                                 ("QuaternionParameter", "FsmQuaternion"), ("EnumParameter", "FsmEnum"), ("ArrayParameter", "FsmArray")):
                        x = v.get(pk)
                        if x is None:
                            continue
                        if x.get("name"):
                            fl[pk] = var_ref(kind, x["name"])
                        elif kind in ("FsmMaterial", "FsmTexture"):
                            fl[pk] = dict(self.named("FsmObject", x, fname), __fsm=kind)
                        else:
                            fl[pk] = self.named(kind, x, fname)
                return decl or "HutongGames.PlayMaker.FunctionCall", unser("HutongGames.PlayMaker.FunctionCall", fl), 0
            if k == "FsmAnimationCurve":
                v = (ad.get("animationCurveParams") or [])[pos]
                return decl or "HutongGames.PlayMaker.FsmAnimationCurve", unser("HutongGames.PlayMaker.FsmAnimationCurve", {"curve": v.get("curve")}), 0
            if k in ("FsmProperty", "LayoutOption", "FsmTemplateControl"):
                lst = {"FsmProperty": "fsmPropertyParams", "LayoutOption": "layoutOptionParams",
                       "FsmTemplateControl": "fsmTemplateControlParams"}[k]
                v = (ad.get(lst) or [])[pos]
                return decl or "HutongGames.PlayMaker." + k, unser("HutongGames.PlayMaker." + k, self.A.resolve(fname, v)), 0
            if k == "Array":
                et = (ad.get("arrayParamTypes") or [])[pos]
                n = (ad.get("arrayParamSizes") or [])[pos]
                vals = []
                j = i
                for _ in range(n):
                    j += 1
                    _t, x, extra = value(j, None, None)
                    vals.append(x)
                    j += extra
                return decl or et + "[]", vals, j - i
            if k == "CustomClass":
                cn = (ad.get("customTypeNames") or [])[pos]
                n = (ad.get("customTypeSizes") or [])[pos]
                fields, j = {}, i
                for _ in range(n):
                    j += 1
                    _t, x, extra = value(j, None, None)
                    fields[pname[j] if j < len(pname) else "#%d" % j] = x
                    j += extra
                return decl or cn, unser(cn, fields), j - i
            self.unknown_types.add(k)
            return decl or "?", {"__unsupported": k}, 0

        for ai, an in enumerate(names):
            end = starts[ai + 1] if ai + 1 < len(starts) else len(pdt)
            fields = []
            i = starts[ai] if ai < len(starts) else len(pdt)
            while i < end:
                nm = pname[i] if i < len(pname) else "#%d" % i
                t, v, extra = value(i, an, nm)
                fields.append({"name": nm, "type": t, "value": v})
                i += 1 + extra
            en = ad.get("actionEnabled") or []
            actions.append({"index": ai, "type": an, "enabled": bool(en[ai]) if ai < len(en) else True, "fields": fields})
        return actions

    def decode(self, comp_oid, go_path, go_name):
        fname = comp_oid.rsplit(":", 1)[0]
        d = self.A.raw(comp_oid)
        fsm = d["fsm"]
        tmpl = self.A.ptr(fname, d["fsmTemplate"]) if d.get("fsmTemplate") else None
        name = fsm.get("name")
        vfname, own_vars = fname, fsm.get("variables") or {}
        if tmpl:
            # PlayMakerFSM.InitTemplate (PlayMakerFSM.cs:183-197): the body is the FsmTemplate's (Fsm(Fsm source):
            # Fsm.cs:1398-1437), the name the component's own, and the template's variables take the component's
            # values where the names match and the template variable is ShowInInspector
            # (FsmVariables.OverrideVariableValues, FsmVariables.cs:578-).
            fsm = self.A.raw(tmpl)["fsm"]
            fname = tmpl.rsplit(":", 1)[0]
            merged = {}
            for _b, key in VAR_LISTS:
                lst = [dict(v) for v in (fsm.get("variables") or {}).get(key) or []]
                for v in lst:
                    for o in own_vars.get(key) or []:
                        if v.get("showInInspector") and o.get("name") == v.get("name"):
                            for k2 in ("value", "floatValues", "intValues", "boolValues", "stringValues", "vector4Values",
                                       "objectReferences", "intValue", "enumName", "typeName"):
                                if k2 in o:
                                    v[k2] = o[k2]
                            v["__from"] = vfname
                lst and merged.setdefault(key, lst)
            own_vars = merged
        self.global_events = set()
        var_index = {}
        vs = {}
        for bucket, key in VAR_LISTS:
            vs[bucket] = [self.named("Fsm" + bucket, v, v.get("__from", fname) if v else fname)
                          for v in own_vars.get(key) or []] if tmpl else None
        if not tmpl:
            vs = self.variables(own_vars, fname)
        for bucket, lst in vs.items():
            for v in lst:
                if v is not None:
                    var_index[("Fsm" + bucket, v["name"])] = v
        # FsmObject-derived references resolve against the Object bucket by name (FsmVariables.GetFsmMaterial etc.)
        for v in vs.get("Object", []):
            for k in ("FsmMaterial", "FsmTexture"):
                var_index.setdefault((k, v["name"]), dict(v, __fsm=k))
        events = [{"name": e["name"], "isGlobal": bool(e.get("isGlobal"))} for e in fsm.get("events") or []]
        self.global_events = {e["name"] for e in events if e["isGlobal"]}

        def trans(t):
            ev = t.get("fsmEvent") or {}
            return {"event": ev.get("name"), "toState": t.get("toState"),
                    "linkStyle": LINKSTYLE[t.get("linkStyle", 0)] if t.get("linkStyle", 0) < len(LINKSTYLE) else t.get("linkStyle"),
                    "isGlobal": bool(ev.get("isGlobal"))}
        states = []
        for st in fsm.get("states") or []:
            states.append({"name": st["name"], "isSequence": bool(st.get("isSequence")),
                           "isBreakpoint": bool(st.get("isBreakpoint")),
                           "transitions": [trans(t) for t in st.get("transitions") or []],
                           "actions": self.decode_state(st, fsm, fname, var_index)})
        return {"path": go_path, "gameObject": go_name, "component": comp_oid, "enabled": bool(d.get("m_Enabled")),
                "fsmName": name, "template": self.A.describe(tmpl)["name"] if tmpl else None,
                "name": name, "description": fsm.get("description"), "dataVersion": fsm.get("dataVersion"),
                "startState": fsm.get("startState"), "activeStateName": fsm.get("activeStateName"),
                "restartOnEnable": bool(fsm.get("RestartOnEnable")), "manualUpdate": bool(fsm.get("manualUpdate")),
                "keepDelayedEventsOnStateExit": bool(fsm.get("keepDelayedEventsOnStateExit")),
                "maxLoopCount": fsm.get("maxLoopCount"), "handleFixedUpdate": bool(fsm.get("handleFixedUpdate")),
                "handleLateUpdate": bool(fsm.get("handleLateUpdate")),
                "handle2d": {k: bool(fsm.get(k)) for k in ("handleTriggerEnter2D", "handleTriggerExit2D", "handleTriggerStay2D",
                                                           "handleCollisionEnter2D", "handleCollisionExit2D", "handleCollisionStay2D")},
                "variables": vs, "events": events, "globalTransitions": [trans(t) for t in fsm.get("globalTransitions") or []],
                "states": states}


# ------------------------------------------------------------------------------------------------ Mecanim

# Serialized property names an AnimationClip binding can name (GenericBinding.attribute = CRC32 of the name for
# non-Transform bindings).  A hash not in this list is written as its number.
ANIM_PROPS = ["m_Enabled", "m_IsActive", "m_Sprite", "m_FlipX", "m_FlipY", "m_SortingOrder"]
for _p in ("m_Color", "m_Offset", "m_Size", "m_LocalPosition", "m_LocalScale", "m_LocalEulerAngles", "m_Radius",
           "m_EdgeRadius", "m_Density"):
    ANIM_PROPS += [_p] + ["%s.%s" % (_p, c) for c in ("x", "y", "z", "w", "r", "g", "b", "a")]
# Transform bindings (typeID 4): attribute 1 position, 2 rotation (quaternion), 3 scale, 4 euler (Unity
# GenericBinding kBindTransform*), each 3 or 4 curves wide.
TRANSFORM_ATTR = {1: ("m_LocalPosition", "xyz"), 2: ("m_LocalRotation", "xyzw"), 3: ("m_LocalScale", "xyz"),
                  4: ("m_LocalEulerAngles", "xyz")}
# Unity class ids of the bound component types that occur in the ported scenes' clips.
CLASS_IDS = {1: "GameObject", 4: "Transform", 23: "MeshRenderer", 50: "Rigidbody2D", 58: "CircleCollider2D",
             60: "PolygonCollider2D", 61: "BoxCollider2D", 68: "EdgeCollider2D", 70: "CapsuleCollider2D",
             82: "AudioSource", 114: "MonoBehaviour", 198: "ParticleSystem", 199: "ParticleSystemRenderer",
             212: "SpriteRenderer"}


def decode_clip(A, clip_oid, path_names):
    """AnimationClip -> curves.  The player build keeps only the compiled muscle clip: a streamed part (keyframes as
    Hermite coefficients, frame by frame), a dense part (sampled frames) and a constant part, whose curves are laid
    out in that order and assigned to m_ClipBindingConstant.genericBindings in binding order (a Transform binding
    takes 3 or 4 curves, every other one 1).  `path_names`: CRC32(path relative to the Animator) -> path."""
    import zlib
    c = A.raw(clip_oid)
    mc = c["m_MuscleClip"]
    cl = mc["m_Clip"]["data"] if "data" in mc["m_Clip"] else mc["m_Clip"]
    sc, dc, cc = cl["m_StreamedClip"], cl["m_DenseClip"], cl["m_ConstantClip"]
    attr_names = {zlib.crc32(p.encode()): p for p in ANIM_PROPS}
    # streamed: uint32 words; frame = time f32, key count u32, keys of (curve index u32, 4 coefficients f32)
    raw = struct.pack("<%dI" % len(sc["data"]), *sc["data"])
    keys = {}
    pos = 0
    while pos + 8 <= len(raw):
        t, n = struct.unpack_from("<fI", raw, pos)
        pos += 8
        for _ in range(n):
            idx, a, b, c3, d = struct.unpack_from("<I4f", raw, pos)
            pos += 20
            keys.setdefault(idx, []).append({"t": t, "coeff": [a, b, c3, d]})
    n_stream = sc.get("curveCount", 0)
    n_dense = dc["m_CurveCount"]
    curves = []
    ci = 0
    fname = clip_oid.rsplit(":", 1)[0]
    for b in c["m_ClipBindingConstant"]["genericBindings"]:
        if b.get("isPPtrCurve"):
            curves.append({"path": path_names.get(b["path"], b["path"]), "class": CLASS_IDS.get(b["typeID"], b["typeID"]),
                           "attribute": attr_names.get(b["attribute"], b["attribute"]), "pptr": True})
            continue
        if b["typeID"] == 4 and b["attribute"] in TRANSFORM_ATTR:
            nm, comps = TRANSFORM_ATTR[b["attribute"]]
            names = ["%s.%s" % (nm, x) for x in comps]
        else:
            names = [attr_names.get(b["attribute"], b["attribute"])]
        for nm in names:
            cur = {"path": path_names.get(b["path"], b["path"]), "class": CLASS_IDS.get(b["typeID"], b["typeID"]),
                   "attribute": nm}
            if b["typeID"] == 114:
                cur["script"] = A.describe(A.ptr(fname, b["script"]))
            if ci < n_stream:
                cur["streamed"] = keys.get(ci, [])
            elif ci < n_stream + n_dense:
                k = ci - n_stream
                arr = dc["m_SampleArray"]
                cur["dense"] = {"begin": dc["m_BeginTime"], "rate": dc["m_SampleRate"],
                                "values": [arr[f * n_dense + k] for f in range(dc["m_FrameCount"]) if f * n_dense + k < len(arr)]}
            else:
                k = ci - n_stream - n_dense
                cur["constant"] = cc["data"][k] if k < len(cc["data"]) else None
            curves.append(cur)
            ci += 1
    ev = [{"time": e["time"], "function": e["functionName"], "string": e.get("data"),
           "float": e.get("floatParameter"), "int": e.get("intParameter"),
           "object": A.describe(A.ptr(fname, e["objectReferenceParameter"]))}
          for e in c.get("m_Events") or []]
    return {"id": clip_oid, "name": c["m_Name"], "sampleRate": c["m_SampleRate"], "wrapMode": c["m_WrapMode"],
            "start": mc["m_StartTime"], "stop": mc["m_StopTime"], "loopTime": bool(mc.get("m_LoopTime")),
            "curves": curves, "events": ev}


TK2D_WRAP = ["Loop", "LoopSection", "Once", "PingPong", "RandomFrame", "RandomLoop", "Single"]   # tk2dSpriteAnimationClip.WrapMode


def decode_tk2d_library(A, lib_oid, collections):
    """tk2dSpriteAnimation -> the dumps' library form (ReflectionDumper.Animator): clips with frames naming their
    sprite collection by object name; every collection a frame uses goes into `collections` with each sprite's
    collider fields (tk2dSpriteDefinition: physicsEngine, colliderType, colliderVertices), the sprites.json form."""
    d = A.raw(lib_oid)
    fname = lib_oid.rsplit(":", 1)[0]
    clips = []
    for i, c in enumerate(d.get("clips") or []):
        frames = []
        for j, fr in enumerate(c.get("frames") or []):
            col = A.ptr(fname, fr["spriteCollection"])
            cname = A.describe(col)["name"] if col else None
            if col and cname not in collections:
                cd = A.raw(col)
                collections[cname] = {"name": cname, "id": col, "spriteCollectionName": cd.get("spriteCollectionName"),
                                      "sprites": [{"id": k, "name": sd.get("name"), "physicsEngine": sd.get("physicsEngine"),
                                                   "colliderType": sd.get("colliderType"),
                                                   "colliderVertices": [[v["x"], v["y"], v["z"]] for v in sd.get("colliderVertices") or []]}
                                                  for k, sd in enumerate(cd.get("spriteDefinitions") or [])]}
            frames.append({"index": j, "spriteId": fr["spriteId"], "triggerEvent": bool(fr["triggerEvent"]),
                           "eventInfo": fr["eventInfo"], "eventInt": fr["eventInt"], "eventFloat": fr["eventFloat"],
                           "spriteCollection": cname})
        wm = c.get("wrapMode", 0)
        clips.append({"id": i, "name": c["name"], "fps": c["fps"], "loopStart": c["loopStart"], "frames": frames,
                      "wrapMode": {"__enum": "tk2dSpriteAnimationClip+WrapMode", "value": wm,
                                   "name": TK2D_WRAP[wm] if 0 <= wm < len(TK2D_WRAP) else None}})
    return {"id": lib_oid, "name": A.describe(lib_oid)["name"], "clips": clips}


def decode_controller(A, ctl_oid):
    """RuntimeAnimatorController: its clips and its compiled state machine (m_Controller, kept verbatim), with the
    name table m_TOS that resolves the state machine's hashes."""
    d = A.raw(ctl_oid)
    if d is None:
        return None
    t = A.type_name(ctl_oid)
    fname = ctl_oid.rsplit(":", 1)[0]
    if t == "AnimatorOverrideController":
        return {"id": ctl_oid, "type": t, "name": d.get("m_Name"),
                "controller": A.describe(A.ptr(fname, d["m_Controller"])),
                "clips": [[A.describe(A.ptr(fname, x["m_OriginalClip"])),
                           A.describe(A.ptr(fname, x["m_OverrideClip"]))] for x in d.get("m_Clips") or []]}
    return {"id": ctl_oid, "type": t, "name": d.get("m_Name"), "tos": {str(h): s for h, s in d.get("m_TOS") or []},
            "clips": [A.ptr(fname, x) for x in d.get("m_AnimationClips") or []],
            "stateMachine": d.get("m_Controller")}


# ------------------------------------------------------------------------------------------------ driver

def iter_refs(v):
    if isinstance(v, dict):
        if "$ref" in v:
            yield v
        for x in v.values():
            yield from iter_refs(x)
    elif isinstance(v, list):
        for x in v:
            yield from iter_refs(x)


def is_level(oid):
    return oid.split(":", 1)[0].startswith("level")


def collect(A, dec, roots):
    """Objects, decoded FSMs and animators of every GameObject under `roots`."""
    import zlib
    objects, fsms, anims = [], [], {"controllers": {}, "clips": {}, "animators": [], "tk2d_libraries": {},
                                    "tk2d_collections": {}}
    for r in roots:
        tree = A.tree(r)
        objects += tree
        for o in tree:
            for c in o["components"]:
                if c.get("script") == "PlayMakerFSM":
                    f = dec.decode(c["id"], o["path"], o["name"])
                    fsms.append(f)
                    c["data"]["fsm"] = {"name": f["fsmName"], "decoded": "fsms.json.gz"}
                if c.get("script") == "tk2dSpriteAnimator":
                    lib = (c["data"].get("library") or {}).get("$ref")
                    if lib and lib not in anims["tk2d_libraries"]:
                        anims["tk2d_libraries"][lib] = decode_tk2d_library(A, lib, anims["tk2d_collections"])
                if c["class"] == "Animator":
                    ctl = (c["data"].get("m_Controller") or {}).get("$ref")
                    # CRC32 of every path below the Animator, relative to it (GenericBinding.path)
                    names = {0: ""}
                    base = o["path"]
                    for x in tree:
                        if x["path"].startswith(base + "/"):
                            rel = x["path"][len(base) + 1:]
                            names[zlib.crc32(rel.encode())] = rel
                    anims["animators"].append({"object": o["id"], "path": o["path"], "controller": ctl,
                                               "enabled": c["data"].get("m_Enabled")})
                    todo = [ctl] if ctl else []
                    while todo:
                        k = todo.pop()
                        if k in anims["controllers"]:
                            continue
                        cd = decode_controller(A, k)
                        anims["controllers"][k] = cd
                        if cd is None:
                            continue
                        if cd["type"] == "AnimatorOverrideController":
                            if cd["controller"]:
                                todo.append(cd["controller"]["$ref"])
                            clips = [x[1]["$ref"] for x in cd["clips"] if x[1]]
                        else:
                            clips = [x for x in cd["clips"] if x]
                        for cl in clips:
                            if cl not in anims["clips"]:
                                anims["clips"][cl] = decode_clip(A, cl, names)
    return objects, fsms, anims


GO_PART = ("GameObject", "Transform", "MonoBehaviour", "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D",
           "EdgeCollider2D", "Rigidbody2D")


def gameobject_refs(objects, fsms, A):
    """Prefab roots referenced from these objects' components and FSMs: a GameObject, or a component's GameObject, that
    lives in an asset file."""
    out = {}
    items = [(o["path"], c) for o in objects for c in o["components"]]
    items += [(f["path"] + " | " + f["fsmName"], f) for f in fsms]
    for where, item in items:
        for r in iter_refs(item):
            oid = r["$ref"]
            if is_level(oid) or r.get("type") not in GO_PART or "path" not in r:
                continue
            if A.reader(oid) is None:
                continue
            go = oid if r["type"] == "GameObject" else A.ptr(oid.rsplit(":", 1)[0], A.raw(oid)["m_GameObject"])
            if go is None:
                continue
            out.setdefault(A.root_of(go), set()).add(where)
    return out


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with gzip.open(path, "wt", encoding="utf-8") as fh:
        json.dump(obj, fh, separators=(",", ":"))


def prefab_key(A, root):
    name = A.raw(root)["m_Name"]
    safe = re.sub(r'[<>:"/\\|?*]', "_", name)
    return "%s@%s" % (safe, root.replace(":", "-"))


def scene_levels():
    """Build-settings scene name -> level file (globalgamemanagers BuildSettings.scenes index)."""
    import UnityPy
    env = UnityPy.load(os.path.join(GAME, "globalgamemanagers"))
    for o in env.objects:
        if o.type.name == "BuildSettings":
            sc = o.parse_as_dict()["scenes"]
            return {os.path.splitext(os.path.basename(s))[0]: "level%d" % i for i, s in enumerate(sc)}
    raise SystemExit("extract_assets: no BuildSettings in globalgamemanagers")


# DontDestroyOnLoad roots every boss scene runs with; they are not in the level files.  Found as the one prefab root
# of that name whose subtree carries the named component.
DDOL_ROOTS = [("Knight", "HeroController"), ("_GameManager", "GameManager"), ("_GameCameras", "GameCameras")]


def find_ddol_roots(A):
    out = []
    for name, comp in DDOL_ROOTS:
        hits = []
        for fname, sf in A.files.items():
            if fname.startswith("level") or fname == "globalgamemanagers":
                continue
            for pid, o in sf.objects.items():
                if o.type.name != "GameObject":
                    continue
                oid = A.oid(fname, pid)
                if A.raw(oid)["m_Name"] != name or A.root_of(oid) != oid:
                    continue
                if any(c.get("script") == comp for x in A.tree(oid) for c in x["components"]):
                    hits.append(oid)
        if len(hits) != 1:
            raise SystemExit("extract_assets: DDOL root %s: %d prefab candidates carry %s (%s)" % (name, len(hits), comp, hits))
        out.append(hits[0])
    return out


def materials(A, objects):
    """Every PhysicsMaterial2D a Collider2D or Rigidbody2D of these objects names: {id: {name, friction, bounciness}}
    (PhysicsMaterial2D m_Friction / m_Bounciness)."""
    out = {}
    for o in objects:
        for c in o["components"]:
            if c["class"] == "Rigidbody2D" or c["class"].endswith("Collider2D"):
                m = (c.get("data") or {}).get("m_Material")
                if m and m.get("$ref") and m["$ref"] not in out:
                    d = A.raw(m["$ref"]) or {}
                    out[m["$ref"]] = {"name": d.get("m_Name"), "friction": d.get("friction", d.get("m_Friction")),
                                      "bounciness": d.get("bounciness", d.get("m_Bounciness"))}
    return out


def physics2d_settings(A):
    """globalgamemanagers Physics2DSettings (gravity, iterations, default material, the layer matrix) verbatim, with
    the default material's values."""
    o = next(o for o in A.files["globalgamemanagers"].objects.values() if o.type.name == "Physics2DSettings")
    d = A.resolve("globalgamemanagers", o.parse_as_dict())
    ref = (d.get("m_DefaultMaterial") or {}).get("$ref")
    if ref and A.raw(ref):
        m = A.raw(ref)
        d["m_DefaultMaterial"]["friction"] = m.get("friction", m.get("m_Friction"))
        d["m_DefaultMaterial"]["bounciness"] = m.get("bounciness", m.get("m_Bounciness"))
    return d


def pool_startup(objects):
    """ObjectPool / PersonalObjectPool startup pools (ObjectPool.cs:84-100, PersonalObjectPool.cs:22-35) on these
    objects."""
    out = []
    for o in objects:
        for c in o["components"]:
            if c.get("script") in ("ObjectPool", "PersonalObjectPool"):
                lst = c["data"].get("startupPools") or c["data"].get("startupPool") or []
                out.append({"owner": o["path"], "component": c["script"], "id": c["id"],
                            "pools": [{"prefab": p.get("prefab"), "size": p.get("size")} for p in lst]})
    return out


# ------------------------------------------------------------------------------------------------ cross-check

def _norm(v):
    """Comparison form: floats to 5 places, references by name, enum display names and dump-only runtime fields
    (tooltip, namedVar...) dropped."""
    if isinstance(v, float):
        return round(v, 5)
    if isinstance(v, dict):
        if "$ref" in v or "instanceID" in v:
            return ("REF", v.get("name"))
        return {k: _norm(x) for k, x in v.items()
                if k not in ("tooltip", "enumName", "__reason", "isGlobal", "namedVar", "namedVarType", "__destroyed",
                             "__dangling") and not (k == "name" and "__enum" in v)}
    if isinstance(v, list):
        return [_norm(x) for x in v]
    return v


def _leaf(a, b):
    """First differing leaf: None (equal), "RUNTIME" (a named variable reference whose current value the dump took at
    SceneReady, or the unnamed None variable an action wrote into) or the key path."""
    if isinstance(a, dict) and isinstance(b, dict):
        if "__fsm" in a and a.get("useVariable"):
            if a.get("name") and b.get("name") == a.get("name"):
                return None if a == b else "RUNTIME"
            if not a.get("name") and not b.get("name"):
                return None if a == b else "RUNTIME"
        for k in sorted(set(a) & set(b)):
            r = _leaf(a[k], b[k])
            if r:
                return r if r == "RUNTIME" else k + "/" + r
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return "len"
        for x, y in zip(a, b):
            r = _leaf(x, y)
            if r:
                return r
        return None
    if a in ("", None) and b in ("", None):
        return None
    return None if a == b else "value"


def compare_fsm(f, g, stats, bad):
    """One decoded FSM against its dump."""
    if [s["name"] for s in f["states"]] != [s["name"] for s in g["states"]]:
        stats["fsm states differ"] += 1
        bad.append(("states", f["path"], f["fsmName"]))
        return
    stats["fsm"] += 1
    for s1, s2 in zip(f["states"], g["states"]):
        if [(t["event"], t["toState"]) for t in s1["transitions"]] != [(t["event"], t["toState"]) for t in s2["transitions"]]:
            stats["transitions differ"] += 1
            bad.append(("transitions", f["path"], f["fsmName"], s1["name"]))
        if [a["type"] for a in s1["actions"]] != [a["type"] for a in s2.get("actions") or []]:
            stats["action lists differ"] += 1
            bad.append(("actions", f["path"], f["fsmName"], s1["name"]))
            continue
        for a1, a2 in zip(s1["actions"], s2["actions"]):
            stats["actions"] += 1
            stats["enabled equal" if a1["enabled"] == a2.get("enabled") else "enabled differ"] += 1
            f1 = {x["name"]: x for x in a1["fields"]}
            f2 = {x["name"]: x for x in a2.get("fields") or []}
            for n in sorted(set(f1) | set(f2)):
                if n not in f1 or n not in f2:
                    stats["field only in " + ("dump" if n not in f1 else "asset")] += 1
                    continue
                if f1[n]["type"] != f2[n]["type"]:
                    stats["declared type differs"] += 1
                r = _leaf(_norm(f1[n]["value"]), _norm(f2[n]["value"]))
                if r is None:
                    stats["field equal"] += 1
                elif r == "RUNTIME":
                    stats["field runtime-written"] += 1
                else:
                    stats["field differs"] += 1
                    bad.append(("field", f["path"], f["fsmName"], s1["name"], a1["type"], n, r))
    for bucket, lst in f["variables"].items():
        d = {v["name"]: v for v in g["variables"].get(bucket) or [] if v}
        for v in lst:
            if v is None:
                continue
            stats["variables"] += 1
            if v["name"] not in d:
                stats["variable missing in dump"] += 1


def check(out, scenes):
    """Cross-check the extraction against the runtime dumps; one line per check."""
    import collections
    for s in scenes:
        dump = json.load(open(os.path.join(ROOT, "analysis", "fsm", s + ".json"), encoding="utf-8"))
        byk = collections.defaultdict(list)
        for g in dump["fsms"]:
            byk[(g["path"], g["fsmName"])].append(g)
        st, bad = collections.Counter(), []
        # scene FSMs (the dump's scene-object FSMs, not DDOL ones)
        for f in json.load(gzip.open(os.path.join(out, "scenes", s, "fsms.json.gz"), "rt", encoding="utf-8"))["fsms"]:
            c = [g for g in byk.get((f["path"], f["fsmName"]), []) if g.get("scene") == s]
            if not c:
                st["no dump match (moved or destroyed at runtime)"] += 1
                continue
            compare_fsm(f, c[0], st, bad)
        n = st["field equal"] + st["field runtime-written"] + st["field differs"]
        print("check %s scene fsms: %d matched (%d unmatched, %d with other states), %d actions, fields %d equal + %d "
              "runtime-written + %d differ of %d, enabled %d/%d" % (
                  s, st["fsm"], st["no dump match (moved or destroyed at runtime)"], st["fsm states differ"], st["actions"],
                  st["field equal"], st["field runtime-written"], st["field differs"], n, st["enabled equal"], st["actions"]))
        for b in bad[:4]:
            print("   differs:", b)
    # pooled clones: each "<prefab>(Clone)" FSM in the dumps against the prefab's decoded FSM
    ix = json.load(open(os.path.join(out, "index.json"), encoding="utf-8"))
    by_name = collections.defaultdict(list)
    for k, p in ix["prefabs"].items():
        by_name[p["name"]].append(k)
    st, bad, seen = collections.Counter(), [], set()
    for s in scenes:
        dump = json.load(open(os.path.join(ROOT, "analysis", "fsm", s + ".json"), encoding="utf-8"))
        for g in dump["fsms"]:
            m = re.match(r"_GameManager/GlobalPool/(.+?)\(Clone\)(/.*)?$", g["path"])
            if not m or (m.group(1), m.group(2), g["fsmName"]) in seen:
                continue
            seen.add((m.group(1), m.group(2), g["fsmName"]))
            keys = by_name.get(m.group(1), [])
            if len(keys) != 1:
                st["prefab not unique by name (%d)" % len(keys)] += 1
                continue
            pf = json.load(gzip.open(os.path.join(out, "prefabs", keys[0], "fsms.json.gz"), "rt", encoding="utf-8"))["fsms"]
            rel = m.group(1) + (m.group(2) or "")
            c = [f for f in pf if f["path"] == rel and f["fsmName"] == g["fsmName"]]
            if not c:
                st["no prefab fsm"] += 1
                bad.append(("no prefab fsm", rel, g["fsmName"]))
                continue
            compare_fsm(c[0], g, st, bad)
    n = st["field equal"] + st["field runtime-written"] + st["field differs"]
    print("check pooled-clone fsms vs prefab: %d matched, %d with other states, %d states with other actions, %d "
          "without a prefab fsm, %d actions, fields %d equal + %d runtime-written + %d differ of %d" % (
              st["fsm"], st["fsm states differ"], st["action lists differ"], st["no prefab fsm"], st["actions"],
              st["field equal"], st["field runtime-written"], st["field differs"], n))
    for b in bad[:4]:
        print("   differs:", b)
    # pooled clone hierarchy + colliders: the dumped clone subtree against the prefab subtree
    st, bad = collections.Counter(), []
    for s in scenes:
        h = json.load(gzip.open(os.path.join(ROOT, "analysis", "dumps", s, "hierarchy.json.gz"), "rt", encoding="utf-8"))
        roots = collections.defaultdict(list)
        for o in h["objects"]:
            m = re.match(r"DDOL/_GameManager/GlobalPool/(.+?)\(Clone\)(/.*)?$", o["path"])
            if m:
                roots[m.group(1)].append((m.group(2) or "", o))
        for name, lst in roots.items():
            keys = by_name.get(name, [])
            if len(keys) != 1:
                continue
            po = json.load(gzip.open(os.path.join(out, "prefabs", keys[0], "objects.json.gz"), "rt", encoding="utf-8"))["objects"]
            prel = {o["path"][len(name):]: o for o in po}
            first = {}
            for rel, o in lst:
                first.setdefault(rel, o)
            st["clone families"] += 1
            if set(first) != set(prel):
                st["subtree paths differ"] += 1
                bad.append((s, name, sorted(set(first) ^ set(prel))[:3]))
            for rel, o in first.items():
                p = prel.get(rel)
                if p is None:
                    continue
                st["objects"] += 1
                ok = (p["layer"] == o["layer"] and p["tag"] == o["tag"]
                      and all(abs(p["localScale"][k] - o["localScale"][k]) < 1e-4 for k in "xyz"))
                if rel:     # the root's pose is the spawn pose; children keep the prefab's local pose
                    ok = ok and all(abs(p["localPosition"][k] - o["localPosition"][k]) < 1e-4 for k in "xyz")
                st["object fields equal" if ok else "object fields differ"] += 1
                if not ok:
                    bad.append((s, name, rel, "layer/tag/local pose"))
                pc = [c["class"] if c["class"] != "MonoBehaviour" else c.get("script") for c in p["components"]]
                dc = [c["type"].replace("UnityEngine.", "") for c in o.get("components") or []]
                st["component order equal" if pc == dc else "component order differs"] += 1
                if pc != dc:
                    bad.append((s, name, rel, "components", pc[:6], dc[:6]))
    print("check pooled-clone objects vs prefab: %d families, %d with different subtree paths, %d objects: fields %d "
          "equal / %d differ, component order %d equal / %d differ" % (
              st["clone families"], st["subtree paths differ"], st["objects"], st["object fields equal"],
              st["object fields differ"], st["component order equal"], st["component order differs"]))
    for b in bad[:6]:
        print("   differs:", b)

    # scene objects: the level's objects against the dump's hierarchy (objects the scene never moves or renames)
    st, bad = collections.Counter(), []
    for s in scenes:
        h = json.load(gzip.open(os.path.join(ROOT, "analysis", "dumps", s, "hierarchy.json.gz"), "rt", encoding="utf-8"))
        hp = collections.defaultdict(list)
        for o in h["objects"]:
            hp[o["path"]].append(o)
        so = json.load(gzip.open(os.path.join(out, "scenes", s, "objects.json.gz"), "rt", encoding="utf-8"))["objects"]
        for o in so:
            c = hp.get(o["path"])
            if not c:
                st["scene object not in dump"] += 1
                continue
            if len(c) > 1:
                st["dump path ambiguous"] += 1
                continue
            d = c[0]
            st["scene objects"] += 1
            same = (o["layer"] == d["layer"] and o["tag"] == d["tag"] and o["active"] == d["activeSelf"]
                    and all(abs(o["localScale"][k] - d["localScale"][k]) < 1e-4 for k in "xyz")
                    and all(abs(o["localPosition"][k] - d["localPosition"][k]) < 1e-4 for k in "xyz"))
            st["equal" if same else "differ (runtime-changed)"] += 1
            pc = [c2["class"] if c2["class"] != "MonoBehaviour" else c2.get("script") for c2 in o["components"]]
            dc = [c2["type"].replace("UnityEngine.", "") for c2 in d.get("components") or []]
            st["component order equal" if pc == dc else "component order differs"] += 1
            if pc != dc and len(bad) < 6:
                bad.append((s, o["path"], pc[:6], dc[:6]))
    print("check scene objects vs dump hierarchy: %d matched (%d not in the dump), layer/tag/active/local pose %d equal / "
          "%d differ, component order %d equal / %d differ" % (
              st["scene objects"], st["scene object not in dump"], st["equal"], st["differ (runtime-changed)"],
              st["component order equal"], st["component order differs"]))
    for b in bad[:4]:
        print("   differs:", b)
    # colliders: every scene collider row of a scene object against the extracted component
    st = collections.Counter()
    for s in scenes:
        rows = collections.defaultdict(list)
        for c in json.load(open(os.path.join(ROOT, "analysis", "dumps", s, "scene.json"), encoding="utf-8"))["colliders"]:
            rows[(c["path"], c["type"].replace("UnityEngine.", ""))].append(c)
        so = json.load(gzip.open(os.path.join(out, "scenes", s, "objects.json.gz"), "rt", encoding="utf-8"))["objects"]
        for o in so:
            for comp in o["components"]:
                if not comp["class"].endswith("Collider2D"):
                    continue
                r = rows.get((o["path"], comp["class"]))
                if not r or len(r) > 1:
                    st["no single dump row"] += 1
                    continue
                r, d = r[0], comp["data"]
                st["colliders"] += 1
                ok = bool(d.get("m_IsTrigger")) == bool(r.get("isTrigger"))
                off = d.get("m_Offset") or {}
                ok = ok and all(abs(off.get(k, 0) - (r.get("offset") or {}).get(k, 0)) < 1e-4 for k in "xy")
                if comp["class"] == "BoxCollider2D":
                    ok = ok and all(abs(d["m_Size"][k] - r["size"][k]) < 1e-4 for k in "xy")
                elif comp["class"] == "CircleCollider2D":
                    ok = ok and abs(d["m_Radius"] - r["radius"]) < 1e-4
                st["geometry equal" if ok else "geometry differs"] += 1
    print("check scene colliders vs scene.json: %d compared (%d without one dump row), geometry %d equal / %d differ" % (
        st["colliders"], st["no single dump row"], st["geometry equal"], st["geometry differs"]))
    # startup pools against the dumped clone counts
    sizes = collections.Counter()
    for p in ix["ddol"]["pools"]:
        for e in p["pools"]:
            if e["prefab"]:
                sizes[e["prefab"]["name"]] += e["size"]
    st, bad = collections.Counter(), []
    for s in scenes:
        mine = collections.Counter(sizes)
        for p in ix["scenes"][s]["pools"]:
            for e in p["pools"]:
                if e["prefab"]:
                    mine[e["prefab"]["name"]] += e["size"]
        h = json.load(gzip.open(os.path.join(ROOT, "analysis", "dumps", s, "hierarchy.json.gz"), "rt", encoding="utf-8"))
        dumped = collections.Counter()
        for o in h["objects"]:
            m = re.match(r"DDOL/_GameManager/GlobalPool/(.+?)\(Clone\)$", o["path"])
            if m:
                dumped[m.group(1)] += 1
        for name, n in mine.items():
            st["startup entries"] += 1
            k = "equal" if dumped.get(name, 0) == n else ("more dumped" if dumped.get(name, 0) > n else "fewer dumped")
            st[k] += 1
            if k != "equal" and len(bad) < 6:
                bad.append((s, name, n, dumped.get(name, 0)))
        st["dumped families without a startup pool"] += sum(1 for name in dumped if name not in mine)
    print("check startup pools vs dumped clone counts: %d (scene, prefab) entries: %d equal, %d more dumped, %d fewer "
          "dumped; %d dumped families have no startup pool (spawned before the dump)" % (
              st["startup entries"], st["equal"], st["more dumped"], st["fewer dumped"],
              st["dumped families without a startup pool"]))
    for b in bad[:6]:
        print("   (scene, prefab, startup size, dumped):", b)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenes", nargs="*")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    scenes = a.scenes or sorted({re.sub(r"__T\d$", "", os.path.basename(p))
                                 for p in glob.glob(os.path.join(ROOT, "sim", "generated", "GG_*"))})
    levels = scene_levels()
    missing = [s for s in scenes if s not in levels]
    if missing:
        raise SystemExit("extract_assets: not in the build settings: %s" % missing)
    A = Assets([levels[s] for s in scenes])
    dec = FsmDecoder(A, ActionTypes())
    print("load: %d files, %d scenes (%.0fs)" % (len(A.files), len(scenes), time.time() - t0))
    index = {"unity": UNITY, "game": GAME, "generator": "tools/extract_assets.py", "scenes": {}, "prefabs": {},
             "physics2d": physics2d_settings(A)}
    src_of = {}

    def emit(out_dir, head, roots):
        objs, fsms, anims = collect(A, dec, roots)
        write_json(os.path.join(out_dir, "objects.json.gz"), dict(head, objects=objs, physicsMaterials=materials(A, objs)))
        write_json(os.path.join(out_dir, "fsms.json.gz"), {"fsms": fsms})
        write_json(os.path.join(out_dir, "animation.json.gz"), anims)
        return objs, fsms, anims

    ddol = find_ddol_roots(A)
    objs, fsms, anims = emit(os.path.join(a.out, "ddol"), {"roots": ddol}, ddol)
    refs = gameobject_refs(objs, fsms, A)
    for r in refs:
        src_of.setdefault(r, set()).add("ddol")
    index["ddol"] = {"roots": ddol, "objects": len(objs), "fsms": len(fsms), "animators": len(anims["animators"]),
                     "prefabs": sorted(prefab_key(A, r) for r in refs if r not in ddol), "pools": pool_startup(objs)}
    print("ddol: %d roots, %d objects, %d fsms, %d prefab refs" % (len(ddol), len(objs), len(fsms), len(refs)))
    for s in scenes:
        lv = levels[s]
        roots = []
        for pid, o in A.files[lv].objects.items():
            if o.type.name in ("Transform", "RectTransform"):
                td = A.raw(A.oid(lv, pid))
                if A.ptr(lv, td["m_Father"]) is None:
                    roots.append(A.ptr(lv, td["m_GameObject"]))
        roots.sort(key=lambda r: A.raw(A.transform_of(r)).get("m_RootOrder", 0))
        objs, fsms, anims = emit(os.path.join(a.out, "scenes", s), {"level": lv, "roots": roots}, roots)
        r2 = gameobject_refs(objs, fsms, A)
        for r in r2:
            src_of.setdefault(r, set()).add(s)
        index["scenes"][s] = {"level": lv, "roots": len(roots), "objects": len(objs), "fsms": len(fsms),
                              "animators": len(anims["animators"]), "prefabs": sorted(prefab_key(A, r) for r in r2),
                              "pools": pool_startup(objs)}
        print("%s: %s %d objects, %d fsms, %d animators, %d prefab refs" % (s, lv, len(objs), len(fsms),
                                                                            len(anims["animators"]), len(r2)))
    # the prefab closure: what a spawnable prefab references is spawnable too
    done = set(ddol)
    todo = sorted(r for r in src_of if r not in done)
    while todo:
        r = todo.pop()
        if r in done:
            continue
        done.add(r)
        key = prefab_key(A, r)
        objs, fsms, anims = emit(os.path.join(a.out, "prefabs", key), {"root": r}, [r])
        for r2 in gameobject_refs(objs, fsms, A):
            src_of.setdefault(r2, set()).add(key)
            if r2 not in done:
                todo.append(r2)
        index["prefabs"][key] = {"root": r, "name": A.raw(r)["m_Name"], "objects": len(objs), "fsms": len(fsms),
                                 "animators": len(anims["animators"]), "pools": pool_startup(objs)}
    for key, p in index["prefabs"].items():
        p["referenced_by"] = sorted(src_of.get(p["root"], ()))
    with open(os.path.join(a.out, "index.json"), "w", encoding="utf-8") as fh:
        json.dump(index, fh, indent=1, sort_keys=True)
    print("prefabs: %d; parse errors %d; unknown param types %s (%.0fs)" % (len(done) - len(ddol), len(A.errors),
                                                                           sorted(dec.unknown_types) or "none", time.time() - t0))
    for e in A.errors[:5]:
        print("  parse error:", e)
    if a.check:
        check(a.out, scenes)


if __name__ == "__main__":
    main()
