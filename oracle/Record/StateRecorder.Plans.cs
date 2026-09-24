using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using HutongGames.PlayMaker;
using UnityEngine;

namespace HKOracle.Record
{
	// Compiled field plans: the recorder's reflection encoders (MakeEnc) flattened, per type, into ops whose reads
	// are FastAccess IL and whose writes go to precomputed field slots.  An op reads what the encoder of its
	// declared type reads and puts the same (name, type, value); types without a fast op keep their encoder
	// (K_SLOW).  A string or Unity-object leaf whose reference is the one last put is not re-put: its value is a
	// function of that immutable referent.
	public static partial class StateRecorder
	{
		// K_PRESENT: whether the reference is non-null.  K_EVENT: an FsmEvent as Fmt writes it ("ev:<name>"), re-put
		// when its name reference changes.  K_FLIST: an array or List<T> of float-only structs (Vector2/3/4,
		// Quaternion, Color) as ListEnc writes it, copied from the pinned array.  K_KEYLIST: a list of strings or
		// FsmEvents formatted as PutFormatted writes it, re-formatted only when an element (or an event's name)
		// reference changes: the formatted items are functions of those immutable referents.
		// K_BLOB: an array or List<T> of blittable structs formatted by its encoder, re-formatted only when its bytes
		// change.  K_KEYDICT: a dictionary whose keys and values are strings, Unity objects or FsmEvents, re-formatted
		// only when a key or value reference (an event's name reference) changes.
		private const int K_BITS = 0, K_STR = 1, K_UOBJ = 2, K_SLOW = 3, K_PRESENT = 4, K_EVENT = 5, K_FLIST = 6, K_KEYLIST = 7,
			K_BLOB = 8, K_KEYDICT = 9;

		private sealed class Op
		{
			public string Name, Site;
			public char Wire;
			public int Kind;
			public Func<object, long> Bits;
			public Func<object, object> Get;
			public Enc Slow;
			public int Stride;          // K_FLIST: floats per element
			public bool Events;         // K_KEYLIST: elements are FsmEvents (else strings)
			public FieldInfo Items, Size;   // K_FLIST over a List<T>: its backing array and count
		}

		private sealed class Plan
		{
			public Op[] Ops;
			public Schema Schema;   // slot names and types; a K_SLOW op has type '\0' (its encoder puts by name)
			public Plan(List<Op> ops)
			{
				Ops = ops.ToArray();
				var names = new string[Ops.Length];
				var types = new char[Ops.Length];
				for (int i = 0; i < Ops.Length; i++) { names[i] = Ops[i].Name; types[i] = Ops[i].Kind == K_SLOW ? '\0' : Ops[i].Wire; }
				Schema = new Schema(names, types);
			}
		}

		// ---------------------------------------------------------------- slot schemas
		// A fixed list of (name, wire type) whose slots are resolved once per class (cached on Cls.Tag).
		internal sealed class Schema
		{
			public readonly string[] Names;
			public readonly char[] Types;
			public Schema(string[] names, char[] types) { Names = names; Types = types; }
			// "t:name" entries, t a StateWriter field type
			public Schema(params string[] spec)
			{
				Names = new string[spec.Length];
				Types = new char[spec.Length];
				for (int i = 0; i < spec.Length; i++) { Types[i] = spec[i][0]; Names[i] = spec[i].Substring(2); }
			}
			public int this[string name] { get { int i = Array.IndexOf(Names, name); if (i < 0) throw new ArgumentException(name); return i; } }
		}

		private sealed class SlotCache { public Schema[] Keys = new Schema[4]; public int[][] Vals = new int[4][]; public int N; }

		internal static int[] Slots(StateWriter w, StateWriter.Cls c, Schema s)
		{
			var sc = c.Tag as SlotCache;
			if (sc == null) c.Tag = sc = new SlotCache();
			for (int i = 0; i < sc.N; i++) if (ReferenceEquals(sc.Keys[i], s)) return sc.Vals[i];
			var slots = new int[s.Names.Length];
			for (int i = 0; i < slots.Length; i++)
			{
				if (s.Types[i] == '\0') { slots[i] = -1; continue; }
				try { slots[i] = w.Field(c, s.Names[i], s.Types[i]); }
				catch (Exception e) { slots[i] = -1; Err("field " + c.Name + "." + s.Names[i], e); }
			}
			if (sc.N == sc.Keys.Length) { Array.Resize(ref sc.Keys, sc.N * 2); Array.Resize(ref sc.Vals, sc.N * 2); }
			sc.Keys[sc.N] = s; sc.Vals[sc.N] = slots; sc.N++;
			return slots;
		}

		private static int[] Slots(StateWriter.Cls c, Schema s) => Slots(_w, c, s);

		// ---------------------------------------------------------------- running a plan
		private static void RunPlan(StateWriter.Ent e, object o, Plan p, int depth)
		{
			var slots = Slots(e.Cls, p.Schema);
			var ops = p.Ops;
			for (int i = 0; i < ops.Length; i++)
			{
				var op = ops[i];
				if (slots[i] < 0 && op.Kind != K_SLOW) continue;
				try
				{
					switch (op.Kind)
					{
						case K_BITS: _w.Raw(e, slots[i], op.Bits(o)); break;
						case K_STR:
							{
								object r = op.Get(o);
								int s = slots[i];
								if (!_w.SameRef(e, s, r)) { _w.Raw(e, s, _w.Str(r as string)); _w.Ref(e, s, r); }
								break;
							}
						case K_UOBJ:
							{
								object r = op.Get(o);
								int s = slots[i];
								if (!_w.SameRef(e, s, r)) { _w.Raw(e, s, Iid(r as UnityEngine.Object)); _w.Ref(e, s, r); }
								break;
							}
						case K_PRESENT: _w.Raw(e, slots[i], op.Get(o) != null ? 1 : 0); break;
						case K_EVENT:
							{
								object ev = op.Get(o);
								object nm = ev == null ? null : EV_Name(ev);
								object key = ev == null ? NullKey : nm ?? NullNameKey;
								int s = slots[i];
								if (!_w.SameRef(e, s, key)) { _w.Raw(e, s, _w.Str(ev == null ? "null" : "ev:" + (nm as string))); _w.Ref(e, s, key); }
								break;
							}
						case K_FLIST: PutFloatStructs(e, slots[i], op, op.Get(o)); break;
						case K_KEYLIST:
							{
								object v = op.Get(o);
								if (SameKeys(e, slots[i], v as IList, op.Events)) break;
								SlowOp(e, op, v, depth);
								_w.Ref(e, slots[i], KeysOf(v as IList, op.Events));
								break;
							}
						case K_BLOB:
							{
								object v = op.Get(o);
								if (SameBlob(e, slots[i], op, v)) break;
								SlowOp(e, op, v, depth);
								_w.Ref(e, slots[i], _blobNow);
								_blobNow = null;
								break;
							}
						case K_KEYDICT:
							{
								object v = op.Get(o);
								if (SameDictKeys(e, slots[i], v as IDictionary)) break;
								SlowOp(e, op, v, depth);
								_w.Ref(e, slots[i], _dictNow);
								_dictNow = null;
								break;
							}
						default: SlowOp(e, op, op.Get(o), depth); break;
					}
				}
				catch (Exception ex) { Err(op.Site, ex); }
			}
		}

		// Slow ops are timed exclusive of the slow ops they reach (a plain object's own fields).
		private static long _slowInner;
		private static void SlowOp(StateWriter.Ent e, Op op, object v, int depth)
		{
			long before = _slowInner, t0 = System.Diagnostics.Stopwatch.GetTimestamp();
			try { op.Slow(e, op.Name, v, depth); }
			finally
			{
				long dt = System.Diagnostics.Stopwatch.GetTimestamp() - t0;
				Tally(_siteTicks, op.Site, dt - (_slowInner - before));
				_slowInner = before + dt;
			}
		}

		private static readonly object NullKey = new object(), NullNameKey = new object();
		private static readonly Func<object, object> EV_Name = R(typeof(FsmEvent), "name");

		private static object KeyOf(object x, bool events) => !events ? x : x == null ? NullKey : EV_Name(x) ?? NullNameKey;

		private static object[] KeysOf(IList l, bool events)
		{
			int n = l == null ? 0 : l.Count;
			var k = new object[n];
			for (int i = 0; i < n; i++) k[i] = KeyOf(l[i], events);
			return k;
		}

		private static bool SameKeys(StateWriter.Ent e, int slot, IList l, bool events)
		{
			if (!(_w.RefOf(e, slot) is object[] k)) return false;
			int n = l == null ? 0 : l.Count;
			if (k.Length != n) return false;
			for (int i = 0; i < n; i++) if (!ReferenceEquals(k[i], KeyOf(l[i], events))) return false;
			return true;
		}

		// The array's bytes (length first) against the snapshot last formatted; _blobNow holds this frame's snapshot.
		private static byte[] _blobNow;
		private static bool SameBlob(StateWriter.Ent e, int slot, Op op, object v)
		{
			Array arr = v as Array;
			int count = arr == null ? 0 : arr.Length;
			if (arr == null && v != null)
			{
				arr = op.Items.GetValue(v) as Array;
				count = (int)op.Size.GetValue(v);
			}
			int nb = 4 + count * op.Stride;
			if (_blobScratch.Length < nb) _blobScratch = new byte[nb * 2];
			var snap = _blobScratch;
			snap[0] = (byte)count; snap[1] = (byte)(count >> 8); snap[2] = (byte)(count >> 16); snap[3] = (byte)(count >> 24);
			if (count > 0)
			{
				var h = System.Runtime.InteropServices.GCHandle.Alloc(arr, System.Runtime.InteropServices.GCHandleType.Pinned);
				try { System.Runtime.InteropServices.Marshal.Copy(h.AddrOfPinnedObject(), snap, 4, count * op.Stride); }
				finally { h.Free(); }
			}
			if (_w.RefOf(e, slot) is byte[] last && last.Length == nb)
			{
				bool same = true;
				for (int k = 0; k < nb && same; k++) same = last[k] == snap[k];
				if (same) return true;
			}
			_blobNow = new byte[nb];
			Array.Copy(snap, _blobNow, nb);
			return false;
		}
		private static byte[] _blobScratch = new byte[256];

		private static object[] _dictNow;
		private static bool SameDictKeys(StateWriter.Ent e, int slot, IDictionary d)
		{
			int n = d == null ? 0 : d.Count;
			var keys = new object[2 * n];
			int i = 0;
			if (d != null)
				foreach (DictionaryEntry kv in d)
				{
					if (i + 2 > keys.Length) Array.Resize(ref keys, i + 2);
					keys[i++] = DictKey(kv.Key); keys[i++] = DictKey(kv.Value);
				}
			if (_w.RefOf(e, slot) is object[] last && last.Length == i)
			{
				bool same = true;
				for (int k = 0; k < i && same; k++) same = ReferenceEquals(last[k], keys[k]);
				if (same) return true;
			}
			if (i != keys.Length) Array.Resize(ref keys, i);
			_dictNow = keys;
			return false;
		}
		private static object DictKey(object x) => x is FsmEvent ? KeyOf(x, true) : x ?? NullKey;

		private static bool RefFormatted(Type t) => t == typeof(string) || t == typeof(FsmEvent) || typeof(UnityEngine.Object).IsAssignableFrom(t);

		// A value type with only primitive, enum or such struct fields, other than bool and char (their marshalled
		// size differs from their size in an array).
		private static bool Blittable(Type t)
		{
			if (!t.IsValueType || t.IsPrimitive && (t == typeof(bool) || t == typeof(char)) || t.IsPointer) return false;
			if (t.IsPrimitive || t.IsEnum) return true;
			foreach (var f in t.GetFields(InstAll)) if (!Blittable(f.FieldType)) return false;
			return true;
		}

		private static float[] _fbuf = new float[256];
		private static long[] _lbuf = new long[256];

		private static void PutFloatStructs(StateWriter.Ent e, int slot, Op op, object v)
		{
			Array arr = v as Array;
			int count = arr == null ? 0 : arr.Length;
			if (arr == null && v != null)
			{
				arr = op.Items.GetValue(v) as Array;
				count = (int)op.Size.GetValue(v);
			}
			int ne = Math.Min(count, (NumListCap + op.Stride - 1) / op.Stride), nf = ne * op.Stride;
			if (_fbuf.Length < nf) { _fbuf = new float[nf]; _lbuf = new long[nf]; }
			if (nf > 0)
			{
				var h = System.Runtime.InteropServices.GCHandle.Alloc(arr, System.Runtime.InteropServices.GCHandleType.Pinned);
				try { System.Runtime.InteropServices.Marshal.Copy(h.AddrOfPinnedObject(), _fbuf, 0, nf); }
				finally { h.Free(); }
			}
			for (int j = 0; j < nf; j++) _lbuf[j] = StateWriter.FloatBits(_fbuf[j]);
			_w.RawList(e, slot, _lbuf, nf);
		}

		private static readonly Type[] FloatStructs = { typeof(Vector2), typeof(Vector3), typeof(Vector4), typeof(Quaternion), typeof(Color) };

		// ---------------------------------------------------------------- building plans
		private static readonly Dictionary<Type, Plan> _instPlans = new Dictionary<Type, Plan>();
		private static readonly Dictionary<Type, Plan> _staticPlans = new Dictionary<Type, Plan>();
		private static readonly Dictionary<Type, Plan> _nvPlans = new Dictionary<Type, Plan>();

		// Instance fields from `stop`'s subclasses down to `t` (base first), public and private; a name declared
		// again by a subclass is recorded as "<declaring type>.<name>".
		private static Plan PlanFor(Type t, Type stop)
		{
			if (_instPlans.TryGetValue(t, out Plan plan)) return plan;
			var chain = new List<Type>();
			for (var bt = t; bt != null && bt != stop && bt != typeof(object); bt = bt.BaseType) chain.Insert(0, bt);
			var ops = new List<Op>();
			var seen = new HashSet<string>();
			foreach (var bt in chain)
				foreach (var f in bt.GetFields(InstAll | BindingFlags.DeclaredOnly))
				{
					string name = seen.Add(f.Name) ? f.Name : bt.Name + "." + f.Name;
					BuildOps(ops, name, new[] { f }, f.FieldType, 0, "get " + bt.Name + "." + f.Name);
				}
			plan = new Plan(ops);
			_instPlans[t] = plan;
			return plan;
		}

		// A PlayMaker variable as its own root (the FsmVariables entity): its stored value, as PutNamedVar reads it.
		private static Plan NamedVarPlan(Type nvType, string name)
		{
			var ops = new List<Op>();
			BuildNamedVar(ops, name, new FieldInfo[0], nvType, "var " + name);
			return new Plan(ops);
		}

		private static void BuildOps(List<Op> ops, string n, FieldInfo[] path, Type t, int structDepth, string site)
		{
			if (FastAccess.IsBitsLeaf(t))
			{
				ops.Add(new Op { Name = n, Site = site, Kind = K_BITS, Wire = WireOf(t), Bits = BitsGetter(path) });
				return;
			}
			if (t == typeof(string)) { ops.Add(new Op { Name = n, Site = site, Kind = K_STR, Wire = 's', Get = RefGetter(path) }); return; }
			if (typeof(UnityEngine.Object).IsAssignableFrom(t)) { ops.Add(new Op { Name = n, Site = site, Kind = K_UOBJ, Wire = 'o', Get = RefGetter(path) }); return; }
			if (typeof(NamedVariable).IsAssignableFrom(t)) { BuildNamedVar(ops, n, path, t, site); return; }
			if (t == typeof(FsmEvent)) { ops.Add(new Op { Name = n, Site = site, Kind = K_EVENT, Wire = 's', Get = RefGetter(path) }); return; }
			if (t == typeof(FsmEventTarget))
			{
				// flattened: which FSMs an action sends to is its fields' values (FsmBool/FsmString variables included)
				ops.Add(new Op { Name = n + ".present", Site = site, Kind = K_PRESENT, Wire = 'b', Get = RefGetter(path) });
				foreach (var f in t.GetFields(InstAll)) BuildOps(ops, n + "." + f.Name, Append(path, f), f.FieldType, structDepth, site);
				return;
			}
			// MakeEnc formats an opaque type before looking at it as a collection
			var el = Opaque(t) ? null : ElementType(t);
			if (el != null && Array.IndexOf(FloatStructs, el) >= 0)
			{
				var op = new Op { Name = n, Site = site, Kind = K_FLIST, Wire = 'F', Get = RefGetter(path), Stride = el.GetFields(BindingFlags.Instance | BindingFlags.Public).Length };
				if (!t.IsArray) { op.Items = FindField(t, "_items"); op.Size = FindField(t, "_size"); }
				if (t.IsArray || op.Items != null && op.Size != null) { ops.Add(op); return; }
			}
			if (el != null && el.IsValueType && !el.IsPrimitive && !el.IsEnum && Blittable(el) && Array.IndexOf(FloatStructs, el) < 0)
			{
				var op = new Op { Name = n, Site = site, Kind = K_BLOB, Wire = 'S', Get = RefGetter(path), Slow = MakeEnc(t, structDepth),
					Stride = System.Runtime.InteropServices.Marshal.SizeOf(el) };
				if (!t.IsArray) { op.Items = FindField(t, "_items"); op.Size = FindField(t, "_size"); }
				if (t.IsArray || op.Items != null && op.Size != null) { ops.Add(op); return; }
			}
			if (!Opaque(t) && typeof(IDictionary).IsAssignableFrom(t) && t.IsGenericType && t.GetGenericArguments().Length == 2
				&& RefFormatted(t.GetGenericArguments()[0]) && RefFormatted(t.GetGenericArguments()[1]))
			{
				ops.Add(new Op { Name = n, Site = site, Kind = K_KEYDICT, Wire = 'S', Get = RefGetter(path), Slow = MakeEnc(t, structDepth) });
				return;
			}
			if (el == typeof(string) || el == typeof(FsmEvent))
			{
				ops.Add(new Op { Name = n, Site = site, Kind = K_KEYLIST, Wire = 'S', Get = RefGetter(path), Slow = MakeEnc(t, structDepth), Events = el == typeof(FsmEvent) });
				return;
			}
			if (t == typeof(FsmOwnerDefault))
			{
				var oo = FindField(typeof(FsmOwnerDefault), "ownerOption");
				var og = FindField(typeof(FsmOwnerDefault), "gameObject");
				var gv = og == null ? null : FindField(og.FieldType, "value");
				if (oo != null && og != null && gv != null && FastAccess.IsBitsLeaf(oo.FieldType))
				{
					ops.Add(new Op { Name = n + ".ownerOption", Site = site, Kind = K_BITS, Wire = 'i', Bits = BitsGetter(Append(path, oo)) });
					ops.Add(new Op { Name = n + ".gameObject", Site = site, Kind = K_UOBJ, Wire = 'o', Get = RefGetter(Append(path, og, gv)) });
					return;
				}
			}
			else if (!Opaque(t) && t.IsValueType && !t.IsPointer && structDepth < 3)
			{
				foreach (var f in t.GetFields(InstAll))
				{
					if (f.FieldType == t) continue;
					BuildOps(ops, n + "." + f.Name, Append(path, f), f.FieldType, structDepth + 1, site);
				}
				return;
			}
			ops.Add(new Op { Name = n, Site = site, Kind = K_SLOW, Get = RefGetter(path), Slow = MakeEnc(t, structDepth) });
		}

		// PutNamedVar: FsmEnum as its int, the formatted variable for FsmArray/FsmVar/abstract types, else the
		// declared type's `value` field through that field type's encoder.
		private static void BuildNamedVar(List<Op> ops, string n, FieldInfo[] path, Type declared, string site)
		{
			if (typeof(FsmEnum).IsAssignableFrom(declared))
			{
				var iv = FindField(typeof(FsmEnum), "intValue");
				if (iv != null) { ops.Add(new Op { Name = n, Site = site, Kind = K_BITS, Wire = 'i', Bits = BitsGetter(Append(path, iv)) }); return; }
			}
			else if (!(typeof(FsmArray).IsAssignableFrom(declared) || typeof(FsmVar).IsAssignableFrom(declared) || declared == typeof(NamedVariable) || declared.IsAbstract))
			{
				var vf = ValueField(declared);
				if (vf != null) { BuildOps(ops, n, Append(path, vf), vf.FieldType, 0, site); return; }
			}
			ops.Add(new Op { Name = n, Site = site, Kind = K_SLOW, Get = RefGetter(path), Slow = (e, nm, v, d) => _w.S(e, nm, v == null ? "null" : Fmt(v, 0)) });
		}

		private static char WireOf(Type t)
		{
			if (t == typeof(bool)) return 'b';
			var u = t.IsEnum ? Enum.GetUnderlyingType(t) : t;
			if (u == typeof(float)) return 'f';
			if (u == typeof(double)) return 'd';
			if (u == typeof(long) || u == typeof(ulong)) return 'l';
			return 'i';
		}

		private static FieldInfo[] Append(FieldInfo[] path, params FieldInfo[] more)
		{
			var r = new FieldInfo[path.Length + more.Length];
			Array.Copy(path, r, path.Length);
			Array.Copy(more, 0, r, path.Length, more.Length);
			return r;
		}

		private static FieldInfo FindField(Type t, string name)
		{
			for (var bt = t; bt != null; bt = bt.BaseType)
			{
				var f = bt.GetField(name, InstAll | BindingFlags.DeclaredOnly);
				if (f != null) return f;
			}
			return null;
		}

		// ---------------------------------------------------------------- getters
		// In a nested class so that the static getters of StateRecorder.cs (initialized in the type initializer, in an
		// order across partial files the language leaves open) find it constructed.
		private static class Cache
		{
			public static readonly Dictionary<string, Delegate> Getters = new Dictionary<string, Delegate>();
			public static readonly List<string> Missing = new List<string>();
		}

		private static string PathKey(char kind, FieldInfo[] path)
		{
			var sb = new System.Text.StringBuilder().Append(kind);
			foreach (var f in path) sb.Append('|').Append(f.DeclaringType?.AssemblyQualifiedName).Append("::").Append(f.Name);
			return sb.ToString();
		}

		private static Func<object, long> BitsGetter(FieldInfo[] path)
		{
			string key = PathKey('b', path);
			if (Cache.Getters.TryGetValue(key, out Delegate d)) return (Func<object, long>)d;
			Func<object, long> g;
			try { g = FastAccess.Bits(path); }
			catch (Exception e)
			{
				Log($"IL getter failed for {key}: {e.GetType().Name}: {e.Message}; using reflection");
				var leaf = path[path.Length - 1].FieldType;
				g = o => ToBits(ReflGet(path, o), leaf);
			}
			Cache.Getters[key] = g;
			return g;
		}

		private static Func<object, object> RefGetter(FieldInfo[] path)
		{
			if (path.Length == 0) return o => o;
			string key = PathKey('r', path);
			if (Cache.Getters.TryGetValue(key, out Delegate d)) return (Func<object, object>)d;
			Func<object, object> g;
			var leaf = path[path.Length - 1].FieldType;
			if (leaf.IsPointer || leaf.IsByRef) g = o => ReflGet(path, o);
			else
			{
				try { g = FastAccess.Ref(path); }
				catch (Exception e)
				{
					Log($"IL getter failed for {key}: {e.GetType().Name}: {e.Message}; using reflection");
					g = o => ReflGet(path, o);
				}
			}
			Cache.Getters[key] = g;
			return g;
		}

		private static object ReflGet(FieldInfo[] path, object o)
		{
			object v = o;
			for (int i = 0; i < path.Length; i++)
			{
				if (!path[i].IsStatic && v == null) return null;
				v = path[i].GetValue(path[i].IsStatic ? null : v);
			}
			return v;
		}

		private static long ToBits(object v, Type t)
		{
			if (v == null) return 0;
			var u = t.IsEnum ? Enum.GetUnderlyingType(t) : t;
			if (u == typeof(float)) return StateWriter.FloatBits((float)v);
			if (u == typeof(double)) return BitConverter.DoubleToInt64Bits((double)v);
			if (u == typeof(bool)) return (bool)v ? 1 : 0;
			if (u == typeof(ulong)) return unchecked((long)(ulong)v);
			if (u == typeof(long)) return (long)v;
			if (u == typeof(uint)) return unchecked((int)(uint)v);
			return unchecked((int)Convert.ToInt64(v));
		}

		// A fixed type's field as a reader, for the PlayMaker objects (Fsm, FsmState, FsmStateAction, ...).  The
		// field is looked up from `t` up, so a subclass's field of the same name cannot shadow it.
		private static Func<object, object> R(Type t, string field)
		{
			var f = FindField(t, field);
			if (f == null) { MissingField(t, field); return o => null; }
			return RefGetter(new[] { f });
		}

		private static Func<object, long> B(Type t, string field)
		{
			var f = FindField(t, field);
			if (f == null || !FastAccess.IsBitsLeaf(f.FieldType)) { MissingField(t, field); return o => 0; }
			return BitsGetter(new[] { f });
		}

		private static void MissingField(Type t, string field)
		{
			Log($"field not found: {t.FullName}.{field}");
			Cache.Missing.Add(t.Name + "." + field);
		}
	}
}
