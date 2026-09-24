using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;

namespace HKOracle.Record
{
	// The .hkstate encoder (docs/state-record.md): entities with typed fields, delta-coded frame to frame, one
	// gzip stream.  No Unity dependency, so tests/fixtures/staterec/gen can build the known-answer fixture from
	// this file alone.
	//
	// Model.  A class is a name and an append-only list of (field name, type).  An entity has an eid (1, 2, ...),
	// a class, a parent eid and a key string, all fixed at birth.  Each frame the recorder visits the live
	// entities and puts field values; Commit emits only the fields whose value differs from the entity's last
	// emitted value (all-zero before birth).  An entity not visited in a frame dies at EndFrame.
	public sealed class StateWriter : IDisposable
	{
		public const int Version = 1;

		// record tags
		public const byte T_STR = 1, T_CLASS = 2, T_FRAME = 3, T_BORN = 4, T_DIED = 5, T_SET = 6, T_END = 7,
			T_NOTE = 8, T_TRAILER = 9;

		// field types: scalars f32 bits, f64 bits, i32, i64, bool, string id, Unity instance id, eid;
		// lists of i32, i64, f32 bits, string ids, instance ids, eids
		public const string ScalarTypes = "fdilbsoe", ListTypes = "ILFSOE";

		public sealed class Cls
		{
			public readonly int Id;
			public readonly string Name;
			internal readonly List<string> Names = new List<string>();
			internal readonly List<char> Types = new List<char>();
			internal readonly Dictionary<string, int> Index = new Dictionary<string, int>();
			internal int Emitted;
			internal bool Declared;
			// The recorder's per-class cache (resolved slots of a field plan); the writer does not read it.
			public object Tag;
			internal Cls(int id, string name) { Id = id; Name = name; }
			public int Count => Names.Count;
		}

		public sealed class Ent
		{
			public readonly int Eid;
			public readonly Cls Cls;
			internal readonly long Sig;
			internal long[] Last = new long[8];
			internal long[][] LastL = new long[8][];
			internal long[] Cur = new long[8];
			internal long[][] CurL = new long[8][];
			internal bool[] Put = new bool[8];
			internal object[] Refs = new object[8];
			internal int VisitedFrame = -1;
			internal bool Born;
			internal readonly int Parent;
			internal readonly string KeyStr;
			internal long LKey;
			internal object OKey;
			// The recorder's per-entity cache; the writer does not read it.
			public object Tag;
			public bool IsNew => !Born;
			internal Ent(int eid, Cls cls, long sig, int parent, string key) { Eid = eid; Cls = cls; Sig = sig; Parent = parent; KeyStr = key; }
			internal void Grow(int n)
			{
				if (n <= Cur.Length) return;
				int m = Math.Max(n, Cur.Length * 2);
				Array.Resize(ref Last, m); Array.Resize(ref LastL, m); Array.Resize(ref Cur, m);
				Array.Resize(ref CurL, m); Array.Resize(ref Put, m); Array.Resize(ref Refs, m);
			}
		}

		internal sealed class RefEq : IEqualityComparer<object>
		{
			public new bool Equals(object a, object b) => ReferenceEquals(a, b);
			public int GetHashCode(object o) => RuntimeHelpers.GetHashCode(o);
		}

		private readonly Stream _raw;
		private readonly GZipStream _gz;
		private readonly MemoryStream _buf = new MemoryStream(1 << 16);
		private readonly Dictionary<string, int> _str = new Dictionary<string, int>();
		private readonly Dictionary<string, Cls> _cls = new Dictionary<string, Cls>();
		private readonly Dictionary<long, Ent> _byLong = new Dictionary<long, Ent>();
		private readonly Dictionary<object, Ent> _byRef = new Dictionary<object, Ent>(new RefEq());
		private readonly List<Ent> _all = new List<Ent>();
		private int _nextEid = 1;
		private int _frame = -1;
		private bool _inFrame;

		public long Frames { get; private set; }
		public long Births { get; private set; }
		public long Deaths { get; private set; }
		public long Sets { get; private set; }
		public int LiveEntities => _all.Count;
		public long BytesUncompressed { get; private set; }

		// Key namespaces for long keys (Unity instance ids, native pointers, singletons): the namespace sits in the
		// top byte, the value in the low 56 bits.
		public static long Key(int ns, long v) => ((long)ns << 56) | (v & 0x00FFFFFFFFFFFFFFL);

		// async: the gzip stream is fed by a background thread, so compression and file writes leave the frame.
		public StateWriter(Stream raw, string headerJson, bool async = false)
		{
			_raw = raw;
			if (async)
			{
				_thread = new Thread(Drain) { IsBackground = true, Name = "hkstate writer" };
				_thread.Start();
			}
			_gz = new GZipStream(raw, CompressionLevel.Fastest, true);
			_str[""] = 0;
			byte[] json = Encoding.UTF8.GetBytes(headerJson ?? "{}");
			var w = new BinaryWriter(_buf);
			w.Write(Encoding.ASCII.GetBytes("HKST"));
			w.Write(Version);
			w.Write(json.Length);
			w.Write(json);
			FlushBuf();
		}

		// ------------------------------------------------------------------ strings and classes
		public int Str(string s)
		{
			if (s == null) return 0;
			if (_str.TryGetValue(s, out int id)) return id;
			id = _str.Count;
			_str[s] = id;
			_buf.WriteByte(T_STR);
			Var((ulong)id);
			Utf8(s);
			return id;
		}

		public Cls Class(string name)
		{
			if (_cls.TryGetValue(name, out Cls c)) return c;
			c = new Cls(_cls.Count + 1, name);
			_cls[name] = c;
			return c;
		}

		// Index of `name` in `c`, appended on first use.  A field's type never changes.
		public int Field(Cls c, string name, char type)
		{
			if (c.Index.TryGetValue(name, out int i))
			{
				if (c.Types[i] != type) throw new InvalidOperationException($"field {c.Name}.{name} is '{c.Types[i]}', put as '{type}'");
				return i;
			}
			if (ScalarTypes.IndexOf(type) < 0 && ListTypes.IndexOf(type) < 0) throw new ArgumentException("bad field type " + type);
			i = c.Names.Count;
			c.Names.Add(name); c.Types.Add(type); c.Index[name] = i;
			return i;
		}

		private void EmitClass(Cls c)
		{
			if (c.Emitted == c.Names.Count && c.Declared) return;
			c.Declared = true;
			var names = new int[c.Names.Count];
			for (int i = c.Emitted; i < c.Names.Count; i++) names[i] = Str(c.Names[i]);
			int nameId = Str(c.Name);
			_buf.WriteByte(T_CLASS);
			Var((ulong)c.Id);
			Var((ulong)nameId);
			Var((ulong)c.Emitted);
			Var((ulong)(c.Names.Count - c.Emitted));
			for (int i = c.Emitted; i < c.Names.Count; i++) { Var((ulong)names[i]); _buf.WriteByte((byte)c.Types[i]); }
			c.Emitted = c.Names.Count;
		}

		// ------------------------------------------------------------------ frames
		public void BeginFrame(int frame, int step, int fixedCount, int flags)
		{
			if (_inFrame) throw new InvalidOperationException("BeginFrame inside a frame");
			_inFrame = true;
			_frame++;
			_buf.WriteByte(T_FRAME);
			Var((ulong)(uint)frame);
			Var(Zig(step));
			Var((ulong)(uint)fixedCount);
			_buf.WriteByte((byte)flags);
		}

		public void Note(string text)
		{
			int id = Str(text);
			_buf.WriteByte(T_NOTE);
			Var((ulong)id);
		}

		// Entities alive last frame and not visited this frame die, in eid order.
		public void EndFrame()
		{
			if (!_inFrame) throw new InvalidOperationException("EndFrame outside a frame");
			List<Ent> dead = null;
			foreach (var e in _all) if (e.VisitedFrame != _frame) (dead ?? (dead = new List<Ent>())).Add(e);
			if (dead != null)
			{
				foreach (var e in dead) Kill(e);
				_all.RemoveAll(e => e.VisitedFrame != _frame);
			}
			_buf.WriteByte(T_END);
			_inFrame = false;
			Frames++;
			FlushBuf();
		}

		private void Kill(Ent e)
		{
			if (e.Born)
			{
				_buf.WriteByte(T_DIED);
				Var((ulong)e.Eid);
				Deaths++;
			}
			if (e.OKey != null) _byRef.Remove(e.OKey); else _byLong.Remove(e.LKey);
		}

		// ------------------------------------------------------------------ entities
		// Visit the entity with this key (a long key from Key(), or any object compared by reference).  A key seen
		// with a different class or signature is a new entity: the old one dies first (native memory reused by a
		// new object).  Returns null when the entity was already visited this frame.
		public Ent Visit(long key, Cls cls, long sig, int parent, string keyStr) => VisitImpl(key, null, cls, sig, parent, keyStr);
		public Ent Visit(object key, Cls cls, int parent, string keyStr) => VisitImpl(0, key, cls, 0, parent, keyStr);

		// The eid of an entity visited this frame (or earlier), 0 if unknown.
		public int EidOf(long key) => _byLong.TryGetValue(key, out Ent e) ? e.Eid : 0;
		public int EidOf(object key) => key != null && _byRef.TryGetValue(key, out Ent e) ? e.Eid : 0;
		public bool VisitedThisFrame(long key) => _byLong.TryGetValue(key, out Ent e) && e.VisitedFrame == _frame;
		public bool VisitedThisFrame(object key) => key != null && _byRef.TryGetValue(key, out Ent e) && e.VisitedFrame == _frame;
		// Whether Visit(key, cls, ...) would find an existing entity (so its key string is not needed).
		public bool Known(object key, Cls cls) => key != null && _byRef.TryGetValue(key, out Ent e) && e.Cls == cls;
		public bool Known(long key, Cls cls, long sig) => _byLong.TryGetValue(key, out Ent e) && e.Cls == cls && e.Sig == sig;

		private Ent VisitImpl(long lkey, object okey, Cls cls, long sig, int parent, string keyStr)
		{
			if (!_inFrame) throw new InvalidOperationException("Visit outside a frame");
			Ent e;
			bool found = okey != null ? _byRef.TryGetValue(okey, out e) : _byLong.TryGetValue(lkey, out e);
			if (found && (e.Cls != cls || e.Sig != sig))
			{
				if (e.VisitedFrame == _frame) throw new InvalidOperationException($"key reused within a frame by {cls.Name} (was {e.Cls.Name})");
				Kill(e);
				_all.Remove(e);
				found = false;
			}
			if (found)
			{
				if (e.VisitedFrame == _frame) return null;
				e.VisitedFrame = _frame;
				Array.Clear(e.Put, 0, e.Put.Length);
				return e;
			}
			e = new Ent(_nextEid++, cls, sig, parent, keyStr ?? "") { LKey = lkey, OKey = okey };
			e.VisitedFrame = _frame;
			if (okey != null) _byRef[okey] = e; else _byLong[lkey] = e;
			_all.Add(e);
			return e;
		}

		private int Slot(Ent e, string name, char type)
		{
			int i = Field(e.Cls, name, type);
			e.Grow(i + 1);
			e.Put[i] = true;
			return i;
		}

		public void F(Ent e, string name, float v) { int i = Slot(e, name, 'f'); e.Cur[i] = FloatBits(v); }
		public void D(Ent e, string name, double v) { int i = Slot(e, name, 'd'); e.Cur[i] = BitConverter.DoubleToInt64Bits(v); }
		public void I(Ent e, string name, int v) { int i = Slot(e, name, 'i'); e.Cur[i] = v; }
		public void L(Ent e, string name, long v) { int i = Slot(e, name, 'l'); e.Cur[i] = v; }
		public void B(Ent e, string name, bool v) { int i = Slot(e, name, 'b'); e.Cur[i] = v ? 1 : 0; }
		public void S(Ent e, string name, string v) { int i = Slot(e, name, 's'); e.Cur[i] = Str(v); }
		public void O(Ent e, string name, int iid) { int i = Slot(e, name, 'o'); e.Cur[i] = iid; }
		public void E(Ent e, string name, int eid) { int i = Slot(e, name, 'e'); e.Cur[i] = eid; }
		// lists: `type` is one of ListTypes; values are the raw longs (f32 bits for 'F', string ids for 'S')
		public void List(Ent e, string name, char type, long[] v) { int i = Slot(e, name, type); e.CurL[i] = v ?? new long[0]; }

		// By slot (the index Field returned for the entity's class): a raw scalar (f32/f64 bits, int, bool, id).
		public void Raw(Ent e, int slot, long v) { e.Grow(slot + 1); e.Put[slot] = true; e.Cur[slot] = v; }
		// A list by slot from a scratch buffer: copied only when it differs from the last emitted list.
		public void RawList(Ent e, int slot, long[] buf, int n)
		{
			e.Grow(slot + 1);
			e.Put[slot] = true;
			var last = e.LastL[slot];
			int nl = last == null ? 0 : last.Length;
			bool same = nl == n;
			for (int k = 0; same && k < n; k++) same = last[k] == buf[k];
			if (same) { e.CurL[slot] = last ?? Empty; return; }
			var a = new long[n];
			Array.Copy(buf, a, n);
			e.CurL[slot] = a;
		}
		private static readonly long[] Empty = new long[0];
		// The reference last seen in this slot (for fields whose value is a function of an immutable referent:
		// a string's id, a Unity object's instance id); Ref() stores it.
		public bool SameRef(Ent e, int slot, object r) => slot < e.Refs.Length && ReferenceEquals(e.Refs[slot], r) && e.Born;
		public void Ref(Ent e, int slot, object r) { e.Grow(slot + 1); e.Refs[slot] = r; }
		public object RefOf(Ent e, int slot) => e.Born && slot < e.Refs.Length ? e.Refs[slot] : null;
		public void Strs(Ent e, string name, IList<string> v)
		{
			var a = new long[v == null ? 0 : v.Count];
			for (int k = 0; k < a.Length; k++) a[k] = Str(v[k]);
			List(e, name, 'S', a);
		}

		[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit)]
		private struct FloatInt { [System.Runtime.InteropServices.FieldOffset(0)] public float F; [System.Runtime.InteropServices.FieldOffset(0)] public int I; }
		public static long FloatBits(float f) => (long)(uint)new FloatInt { F = f }.I;

		// Emit the fields put since Visit whose values changed.  A field not put keeps its previous value.
		public void Commit(Ent e)
		{
			if (e == null) return;
			EmitClass(e.Cls);
			if (!e.Born)
			{
				e.Born = true;
				Births++;
				int key = Str(e.KeyStr);
				_buf.WriteByte(T_BORN);
				Var((ulong)e.Eid);
				Var((ulong)e.Cls.Id);
				Var((ulong)e.Parent);
				Var((ulong)key);
			}
			int n = e.Cls.Count;
			e.Grow(n);
			bool opened = false;
			int prev = -1;
			for (int i = 0; i < n; i++)
			{
				if (!e.Put[i]) continue;
				char t = e.Cls.Types[i];
				bool list = ListTypes.IndexOf(t) >= 0;
				if (list ? SameList(e.CurL[i], e.LastL[i]) : e.Cur[i] == e.Last[i]) continue;
				if (!opened)
				{
					opened = true;
					_buf.WriteByte(T_SET);
					Var((ulong)e.Eid);
				}
				Var((ulong)(i - prev));
				prev = i;
				if (list) { WriteList(t, e.CurL[i]); e.LastL[i] = e.CurL[i]; }
				else { WriteScalar(t, e.Cur[i]); e.Last[i] = e.Cur[i]; }
			}
			if (opened) { Var(0); Sets++; }
		}

		private static bool SameList(long[] a, long[] b)
		{
			int na = a == null ? 0 : a.Length, nb = b == null ? 0 : b.Length;
			if (na != nb) return false;
			for (int i = 0; i < na; i++) if (a[i] != b[i]) return false;
			return true;
		}

		private void WriteScalar(char t, long v)
		{
			switch (t)
			{
				case 'f': U32((uint)v); break;
				case 'd': U64((ulong)v); break;
				case 'b': _buf.WriteByte((byte)v); break;
				case 's': case 'e': Var((ulong)v); break;
				case 'i': case 'o': Var(Zig((int)v)); break;
				case 'l': Var(Zig64(v)); break;
				default: throw new InvalidOperationException("scalar type " + t);
			}
		}

		private void WriteList(char t, long[] v)
		{
			Var((ulong)v.Length);
			foreach (long x in v)
			{
				switch (t)
				{
					case 'F': U32((uint)x); break;
					case 'S': case 'E': Var((ulong)x); break;
					case 'I': case 'O': Var(Zig((int)x)); break;
					case 'L': Var(Zig64(x)); break;
					default: throw new InvalidOperationException("list type " + t);
				}
			}
		}

		// ------------------------------------------------------------------ bytes
		private void Var(ulong v)
		{
			while (v >= 0x80) { _buf.WriteByte((byte)(v | 0x80)); v >>= 7; }
			_buf.WriteByte((byte)v);
		}
		private static ulong Zig(int v) => (uint)((v << 1) ^ (v >> 31));
		private static ulong Zig64(long v) => (ulong)((v << 1) ^ (v >> 63));
		private void U32(uint v) { for (int k = 0; k < 4; k++) { _buf.WriteByte((byte)v); v >>= 8; } }
		private void U64(ulong v) { for (int k = 0; k < 8; k++) { _buf.WriteByte((byte)v); v >>= 8; } }
		private void Utf8(string s)
		{
			byte[] b = Encoding.UTF8.GetBytes(s);
			Var((ulong)b.Length);
			_buf.Write(b, 0, b.Length);
		}

		private void FlushBuf()
		{
			if (_buf.Length == 0) return;
			BytesUncompressed += _buf.Length;
			if (_thread == null) _gz.Write(_buf.GetBuffer(), 0, (int)_buf.Length);
			else
			{
				var chunk = _buf.ToArray();
				lock (_q) { _q.Enqueue(chunk); Monitor.Pulse(_q); }
			}
			_buf.SetLength(0);
		}

		private readonly Thread _thread;
		private readonly Queue<byte[]> _q = new Queue<byte[]>();
		private bool _closing;
		public Exception WriterError { get; private set; }

		private void Drain()
		{
			while (true)
			{
				byte[] chunk;
				lock (_q)
				{
					while (_q.Count == 0 && !_closing) Monitor.Wait(_q);
					if (_q.Count == 0) return;
					chunk = _q.Dequeue();
				}
				try { if (WriterError == null) _gz.Write(chunk, 0, chunk.Length); }
				catch (Exception e) { WriterError = e; }
			}
		}

		private void StopThread()
		{
			if (_thread == null) return;
			lock (_q) { _closing = true; Monitor.Pulse(_q); }
			_thread.Join();
		}

		// Trailer JSON (end-of-episode summary), then close the gzip stream and the raw stream.
		public void Close(string trailerJson)
		{
			if (_inFrame) EndFrame();
			int id = Str(trailerJson ?? "{}");
			_buf.WriteByte(T_TRAILER);
			Var((ulong)id);
			FlushBuf();
			StopThread();
			_gz.Dispose();
			_raw.Dispose();
			if (WriterError != null) throw new IOException("hkstate writer thread failed", WriterError);
		}

		public void Dispose() { try { StopThread(); } catch { } try { _gz.Dispose(); } catch { } try { _raw.Dispose(); } catch { } }
	}
}
