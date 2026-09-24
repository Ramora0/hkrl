using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using HutongGames.PlayMaker;
using MonoMod.RuntimeDetour;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Dump;
using HKOracle.Env;

namespace HKOracle.Record
{
	// Counts every Fsm.Update / FixedUpdate / LateUpdate the game performs per FSM per episode,
	// with the owner's full hierarchy path and its state at the first tick and at flush.  One JSON
	// line per episode: <trace>.fsmticks.jsonl.  Installed in record mode (Mode.cs);
	// HK_ORACLE_FSMTICKS=0 disables it so a reference trace can prove the detours inert.
	//
	// Why: gen_tables.py selects the simulator's live FSMs by hand-written rules (LIVE_RULES,
	// derive_boss_live, derive_pool_live); an FSM the game ticks that the rules miss is an attack
	// that spawns nothing in the sim.  This is the measurement those rules are checked against.
	public static class FsmTickRecorder
	{
		private sealed class C { public int upd, fixd, late; public string owner, fsm, first, last; public bool enabledAtFirst; }

		private static readonly Dictionary<Fsm, C> _c = new Dictionary<Fsm, C>();
		private static readonly List<Hook> _hooks = new List<Hook>();
		private static string _path;
		private static int _episode;
		private static bool _armed;

		private static void Log(string m) => HKOracle.Instance.Log("[FsmTicks] " + m);

		public static void Install()
		{
			if (System.Environment.GetEnvironmentVariable("HK_ORACLE_FSMTICKS") == "0") { Log("disabled (HK_ORACLE_FSMTICKS=0)"); return; }
			_path = Mode.SidePath(".fsmticks.jsonl");
			try
			{
				string dir = Path.GetDirectoryName(_path);
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				File.WriteAllText(_path, "");
			}
			catch (Exception e) { Log($"cannot open {_path}: {e.Message}"); return; }

			Hooks.SceneReady += ctx => { Flush("scene_ready_before_arm"); _c.Clear(); _armed = true; };
			Hooks.EpisodeEnd += info => { Flush(info); _c.Clear(); _armed = false; };
			Application.quitting += () => Flush("quit");

			HookTick("Update", 0);
			HookTick("FixedUpdate", 1);
			HookTick("LateUpdate", 2);
			Log($"writing {_path} ({_hooks.Count} tick hooks)");
		}

		private static void HookTick(string name, int which)
		{
			var m = typeof(Fsm).GetMethod(name, BindingFlags.Instance | BindingFlags.Public, null, Type.EmptyTypes, null);
			if (m == null) { Log("UNHOOKED Fsm." + name); return; }
			_hooks.Add(new Hook(m, new Action<Action<Fsm>, Fsm>((orig, self) =>
			{
				orig(self);
				if (_armed) Count(self, which);
			})));
		}

		private static void Count(Fsm f, int which)
		{
			try
			{
				C c;
				if (!_c.TryGetValue(f, out c))
				{
					c = new C { owner = OwnerPath(f), fsm = f.Name ?? "", first = SafeState(f) };
					_c[f] = c;
				}
				if (which == 0) c.upd++; else if (which == 1) c.fixd++; else c.late++;
				c.last = SafeState(f);
			}
			catch { }
		}

		private static string SafeState(Fsm f) { try { return f.ActiveStateName ?? ""; } catch { return ""; } }

		private static string OwnerPath(Fsm f)
		{
			try
			{
				var go = f.GameObject;
				if (go == null) return f.GameObjectName ?? "";
				return ReflectionDumper.Path(go.transform);
			}
			catch { try { return f.GameObjectName ?? ""; } catch { return ""; } }
		}

		private static void Flush(string why)
		{
			if (_c.Count == 0) return;
			try
			{
				var arr = new JArray();
				foreach (var kv in _c)
				{
					var c = kv.Value;
					arr.Add(new JObject { ["owner"] = c.owner, ["fsm"] = c.fsm, ["update"] = c.upd, ["fixed"] = c.fixd, ["late"] = c.late,
						["first_state"] = c.first, ["last_state"] = c.last });
				}
				var o = new JObject { ["episode"] = _episode++, ["why"] = why, ["n"] = arr.Count, ["fsms"] = arr };
				File.AppendAllText(_path, o.ToString(Formatting.None) + "\n");
				Log($"flushed episode {_episode - 1} ({why}): {arr.Count} FSMs ticked");
			}
			catch (Exception e) { Log("flush failed: " + e.Message); }
		}
	}
}
