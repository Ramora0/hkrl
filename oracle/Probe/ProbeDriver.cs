using System;
using System.Collections;
using System.IO;
using HKOracle.Env;
using Modding;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace HKOracle.Probe
{
	// Probe mode (Mode.Probe: HK_ORACLE_PROBE set).  Drives TrainingEnv through init -> reset of HK_ORACLE_LEVEL
	// (regime R2, as the dump driver does), then at SceneReady runs the conformance scenarios of
	// HK_ORACLE_PROBE_SPEC whose name matches HK_ORACLE_PROBE ("all", a name, a comma list, or a prefix ending
	// in '*'), writes HK_ORACLE_PROBE_OUT and quits.  Tool: tools/conformance.py.
	public static class ProbeDriver
	{
		// Far from every arena's colliders; the scenario layer collides with itself only.
		private static readonly Vector2 kOrigin = new Vector2(500f, 500f);
		private const int kLayer = 31;

		private static string _level, _specPath, _outPath, _select;
		private static bool _resetSent, _started;

		private static void Log(string m) => HKOracle.Instance.Log("[Probe] " + m);

		public static void Install()
		{
			_select = Mode.Get("HK_ORACLE_PROBE");
			_level = Mode.Get("HK_ORACLE_LEVEL") ?? "GG_Hornet_1";
			_specPath = Mode.Get("HK_ORACLE_PROBE_SPEC");
			_outPath = Mode.Get("HK_ORACLE_PROBE_OUT");
			if (_specPath == null || _outPath == null || Hooks.Env == null)
			{
				Log("FATAL: HK_ORACLE_PROBE_SPEC and HK_ORACLE_PROBE_OUT are required");
				Application.Quit(1);
				return;
			}
			ProbeStages.Install();
			Socket.Sink = OnOutgoing;
			Hooks.SceneReady += OnSceneReady;
			Hooks.Env.socket.UnreadMessages.Enqueue(new Message { type = "init", data = new MessageData() });
			Log($"probe mode: select={_select} level={_level} spec={_specPath} out={_outPath}");
		}

		private static void OnOutgoing(Message m)
		{
			if (m?.type != "init" || _resetSent) return;
			_resetSent = true;
			Hooks.Env.socket.UnreadMessages.Enqueue(new Message
				{ type = "reset", data = new MessageData { level = _level, frames_per_wait = 2 } });
		}

		private static void OnSceneReady(Hooks.SceneContext ctx)
		{
			if (_started) return;
			_started = true;
			// the knight idles through the scenarios: no damage (HeroController.TakeDamage returns at damage <= 0,
			// HeroController.cs:1825-1829)
			ModHooks.TakeDamageHook += (ref int hazardType, int damage) => 0;
			// a quiet world: the boss's collider toggles would create fixtures, and any fixture creation sets Box2D's
			// world-wide e_newFixture flag, which moves pair discovery of unrelated bodies by one step
			// (analysis/upstream/box2d-v2.3.1/Box2D/Box2D/Dynamics/b2World.cpp:902-906)
			foreach (var hm in ctx.BossHMs) if (hm != null) hm.gameObject.SetActive(false);
			for (int j = 0; j < 32; j++) Physics2D.IgnoreLayerCollision(kLayer, j, j != kLayer);
			var host = new GameObject("conf_host").AddComponent<ProbeHost>();
			UnityEngine.Object.DontDestroyOnLoad(host.gameObject);
			host.StartCoroutine(RunAll());
		}

		private static bool Selected(string name)
		{
			if (_select == "all") return true;
			foreach (var s in _select.Split(','))
			{
				string t = s.Trim();
				if (t.EndsWith("*") ? name.StartsWith(t.Substring(0, t.Length - 1)) : name == t) return true;
			}
			return false;
		}

		private static IEnumerator RunAll()
		{
			var results = new JArray();
			JObject spec = null;
			try { spec = JObject.Parse(File.ReadAllText(_specPath)); }
			catch (Exception e) { Log("FATAL: cannot read spec: " + HKOracle.DescribeException(e)); }
			int onLayer = 0;
			foreach (var g in Resources.FindObjectsOfTypeAll<GameObject>()) if (g.layer == kLayer && g.scene.isLoaded) onLayer++;
			if (spec != null)
				foreach (JObject s in (JArray)spec["scenarios"])
				{
					if (!Selected((string)s["name"])) continue;
					var sc = new Scenario(s, kOrigin, kLayer);
					try { sc.Build(); }
					catch (Exception e) { Log($"{sc.Name}: build failed: {HKOracle.DescribeException(e)}"); }
					int end = sc.Frame0 + ((string)s["shape"]).Length;
					while (Time.frameCount < end) yield return null;
					sc.Close();
					Time.timeScale = 0f;
					sc.Teardown();
					results.Add(sc.Result());
					Log($"{sc.Name}: done");
					for (int i = 0; i < 3; i++) yield return null;
				}
			var outp = new JObject
			{
				["version"] = 1, ["level"] = _level, ["unity"] = Application.unityVersion,
				["mod_commit"] = ModInfo.Commit, ["layer"] = kLayer, ["objects_on_layer"] = onLayer,
				["capture_dt"] = Time.captureDeltaTime, ["fixed_dt"] = Time.fixedDeltaTime,
				["scenarios"] = results,
			};
			try
			{
				string dir = Path.GetDirectoryName(_outPath);
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				File.WriteAllText(_outPath, outp.ToString(Newtonsoft.Json.Formatting.None));
				Log($"wrote {results.Count} scenarios -> {_outPath}");
			}
			catch (Exception e) { Log("FATAL: cannot write output: " + HKOracle.DescribeException(e)); }
			Hooks.Env.socket.UnreadMessages.Enqueue(new Message { type = "close", data = new MessageData() });
			for (int i = 0; i < 30; i++) yield return null;
			Application.Quit(0);
		}
	}

	public class ProbeHost : MonoBehaviour { }
}
