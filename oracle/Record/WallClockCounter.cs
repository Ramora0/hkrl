using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using HKOracle.Env;
using HutongGames.PlayMaker;
using Mono.Cecil;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using UnityEngine;
using CilOpCodes = Mono.Cecil.Cil.OpCodes;

namespace HKOracle.Record
{
	// Which wall-clock call sites (WallClockSites: the list tools/wallclock_sites.py prints) fire during a
	// recorded fight, and in which object. Record mode with HK_ORACLE_WALLCLOCK=1 (Mode.cs); writes
	// <trace base>.wallclock.jsonl:
	//   {"ev":"site","id":i,"m":"Type.Method","k":k,"api":...,"class":...}           one per hooked site
	//   {"ev":"count","ep":e,"phase":"load"|"fight"|"post","id":i,"o":owner,"n":calls,"f0":first frame}
	// flushed at each ResetBegin and at quit. phase: load = ResetBegin..SceneReady, fight = SceneReady..episode
	// end, post = after it. owner: "path|fsm|state" for a PlayMaker action, the object path for a component,
	// "" for a static or plain-class method. Each hooked call gets `Hit(this|null, id)` right before it; the
	// call itself is untouched.
	public static class WallClockCounter
	{
		private static StreamWriter _w;
		private static readonly List<WallClockSites.Site> _sites = new List<WallClockSites.Site>();
		private static readonly List<ILHook> _hooks = new List<ILHook>();
		private sealed class Tally { public int N; public int F0; public string Owner; }
		private static readonly Dictionary<long, Tally> _tally = new Dictionary<long, Tally>();
		private static readonly Dictionary<object, int> _ownerIds = new Dictionary<object, int>(ReferenceEqualityComparer.Instance);
		private static int _episode = -1;
		private static string _phase = "load";
		private static MethodInfo _hit;

		private static void Log(string m) => HKOracle.Instance.Log("[WallClock] " + m);

		public static void Install()
		{
			string path = Mode.SidePath(".wallclock.jsonl");
			_w = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
			_hit = typeof(WallClockCounter).GetMethod(nameof(Hit), BindingFlags.Public | BindingFlags.Static);
			var managed = Path.GetDirectoryName(typeof(GameManager).Assembly.Location);
			var resolver = new DefaultAssemblyResolver();
			resolver.AddSearchDirectory(managed);
			_sites.AddRange(WallClockSites.Scan(WallClockSites.GamePaths(managed), resolver));

			var byMethod = new Dictionary<string, List<int>>();
			for (int i = 0; i < _sites.Count; i++)
			{
				if (!byMethod.TryGetValue(_sites[i].Full, out var l)) byMethod[_sites[i].Full] = l = new List<int>();
				l.Add(i);
			}
			var asms = new Dictionary<string, Assembly>();
			foreach (var a in AppDomain.CurrentDomain.GetAssemblies()) asms[a.GetName().Name] = a;
			int hooked = 0;
			var failed = new List<string>();
			foreach (var kv in byMethod)
			{
				var first = _sites[kv.Value[0]];
				MethodBase mb = null;
				try { mb = asms[first.Assembly].ManifestModule.ResolveMethod(first.Token); } catch { }
				if (mb == null || mb.IsGenericMethodDefinition || (mb.DeclaringType != null && mb.DeclaringType.ContainsGenericParameters))
				{
					failed.Add(first.Full);
					continue;
				}
				var ids = kv.Value;
				bool objThis = !mb.IsStatic && !mb.IsConstructor && mb.DeclaringType != null && !mb.DeclaringType.IsValueType;
				try
				{
					_hooks.Add(new ILHook(mb, il => Manipulate(il, ids, objThis)));
					hooked++;
				}
				catch (Exception e) { failed.Add(first.Full + " (" + e.GetType().Name + ")"); }
			}
			foreach (var s in _sites)
			{
				var sb = new StringBuilder("{\"ev\":\"site\",\"id\":").Append(_sites.IndexOf(s)).Append(",\"m\":");
				WallClockSites.Str(sb, s.Method);
				sb.Append(",\"k\":").Append(s.K).Append(",\"api\":");
				WallClockSites.Str(sb, s.Api);
				sb.Append(",\"class\":");
				WallClockSites.Str(sb, s.Class);
				_w.WriteLine(sb.Append('}').ToString());
			}
			foreach (var f in failed)
			{
				var sb = new StringBuilder("{\"ev\":\"unhooked\",\"full\":");
				WallClockSites.Str(sb, f);
				_w.WriteLine(sb.Append('}').ToString());
			}
			_w.Flush();

			Hooks.ResetBegin += lvl => { Flush(); _phase = "load"; };
			Hooks.SceneReady += ctx => { _episode = ctx.ResetCount; _phase = "fight"; };
			Hooks.EpisodeEnd += info => _phase = "post";
			var go = new GameObject("HKOracle.WallClockCounter");
			UnityEngine.Object.DontDestroyOnLoad(go);
			go.hideFlags = HideFlags.HideAndDontSave;
			go.AddComponent<QuitFlush>();
			Log($"{_sites.Count} sites, {hooked} methods hooked, {failed.Count} unhooked; writing {path}");
			GC.Collect();
		}

		// Sites are numbered in Scan order, which is IL order within a method: the k-th wall-clock call of the
		// method body is ids[k].
		private static void Manipulate(ILContext il, List<int> ids, bool objThis)
		{
			var c = new ILCursor(il);
			int k = 0;
			while (k < ids.Count && c.TryGotoNext(MoveType.AfterLabel, i =>
				(i.OpCode.Code == Mono.Cecil.Cil.Code.Call || i.OpCode.Code == Mono.Cecil.Cil.Code.Callvirt || i.OpCode.Code == Mono.Cecil.Cil.Code.Newobj)
				&& (WallClockSites.IsWallClock(i.Operand as MethodReference, out _, out _) || IsRegimePin(i.Operand as MethodReference))))
			{
				c.Emit(objThis ? CilOpCodes.Ldarg_0 : CilOpCodes.Ldnull);
				c.Emit(CilOpCodes.Ldc_I4, ids[k++]);
				c.Emit(CilOpCodes.Call, _hit);
				c.Index++;
			}
		}

		// A site RegimeClock rewrote before this hook ran: AudioSource.get_isPlaying -> RegimeClock.IsPlaying.
		private static bool IsRegimePin(MethodReference mr) =>
			mr != null && mr.Name == nameof(RegimeClock.IsPlaying) && mr.DeclaringType.FullName == typeof(RegimeClock).FullName;

		public static void Hit(object self, int site)
		{
			try
			{
				object owner = Owner(self);
				int oid = 0;
				if (owner != null && !_ownerIds.TryGetValue(owner, out oid)) _ownerIds[owner] = oid = _ownerIds.Count + 1;
				long key = ((long)site << 32) | (uint)oid | (_phase == "fight" ? 0L : _phase == "load" ? 1L << 30 : 1L << 29);
				if (!_tally.TryGetValue(key, out var t))
					_tally[key] = t = new Tally { F0 = Time.frameCount, Owner = OwnerName(owner) };
				t.N++;
			}
			catch { }
		}

		// An iterator's / closure's `this` is its <>4__this.
		private static object Owner(object self)
		{
			if (self == null) return null;
			var t = self.GetType();
			if (t.IsNested && t.Name.StartsWith("<", StringComparison.Ordinal))
			{
				var f = t.GetField("<>4__this", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
				return f?.GetValue(self);
			}
			return self;
		}

		private static string OwnerName(object o)
		{
			try
			{
				if (o is FsmStateAction a)
				{
					var fs = a.Fsm;
					return (fs?.GameObject != null ? PathOf(fs.GameObject.transform) : "") + "|" + (fs?.Name ?? "") + "|" + (a.State?.Name ?? "");
				}
				if (o is Component comp && comp != null) return PathOf(comp.transform);
				if (o is GameObject g && g != null) return PathOf(g.transform);
			}
			catch { }
			return "";
		}

		private static string PathOf(Transform t)
		{
			var s = t.name;
			for (var p = t.parent; p != null; p = p.parent) s = p.name + "/" + s;
			return s;
		}

		private static void Flush()
		{
			if (_w == null) return;
			foreach (var kv in _tally)
			{
				int site = (int)(kv.Key >> 32);
				long lo = kv.Key & 0xffffffffL;
				string phase = (lo & (1L << 30)) != 0 ? "load" : (lo & (1L << 29)) != 0 ? "post" : "fight";
				var sb = new StringBuilder("{\"ev\":\"count\",\"ep\":").Append(_episode).Append(",\"phase\":\"").Append(phase)
					.Append("\",\"id\":").Append(site).Append(",\"o\":");
				WallClockSites.Str(sb, kv.Value.Owner);
				sb.Append(",\"n\":").Append(kv.Value.N).Append(",\"f0\":").Append(kv.Value.F0).Append('}');
				_w.WriteLine(sb.ToString());
			}
			_tally.Clear();
			_ownerIds.Clear();
			_w.Flush();
		}

		private sealed class QuitFlush : MonoBehaviour
		{
			private void OnApplicationQuit() { Flush(); _w?.Dispose(); _w = null; }
		}

		private sealed class ReferenceEqualityComparer : IEqualityComparer<object>
		{
			public static readonly ReferenceEqualityComparer Instance = new ReferenceEqualityComparer();
			public new bool Equals(object a, object b) => ReferenceEquals(a, b);
			public int GetHashCode(object o) => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(o);
		}
	}
}
