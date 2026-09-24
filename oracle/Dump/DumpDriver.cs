using System;
using System.Collections;
using HKOracle.Game;
using HKOracle.Env;
using UnityEngine;

namespace HKOracle.Dump
{
	// Dump mode (Mode.Dump: HK_ORACLE_DUMPS set, HK_ORACLE_SCRIPT not): no server.
	// Drives TrainingEnv through init -> reset itself via Socket.Sink + UnreadMessages,
	// runs the dumpers at the first SceneReady, then closes the env and quits.
	public static class DumpDriver
	{
		private static string _dumpRoot;
		private static string _level;
		private static bool _resetSent;
		private static bool _dumped;

		private static void Log(string m) => HKOracle.Instance.Log("[Dump] " + m);

		public static void Install()
		{
			_dumpRoot = Mode.DumpRoot;
			_level = System.Environment.GetEnvironmentVariable("HK_ORACLE_LEVEL");
			if (string.IsNullOrEmpty(_level))
			{
				// Guessing a scene would silently produce a dump labelled with
				// the wrong fight.
				Log("FATAL: HK_ORACLE_DUMPS is set but HK_ORACLE_LEVEL is not. "
					+ "Refusing to guess a boss scene. Quitting.");
				Quit(1);
				return;
			}
			if (Hooks.Env == null) { Log("FATAL: Hooks.Env is null at Install()"); Quit(1); return; }

			Log($"dump mode ON: root={_dumpRoot} level={_level}");
			Socket.Sink = OnOutgoing;
			Hooks.SceneReady += OnSceneReady;
			Enqueue("init", new MessageData());
			Log("enqueued init");
		}

		private static void Enqueue(string type, MessageData d)
		{
			Hooks.Env.socket.UnreadMessages.Enqueue(new Message { type = type, data = d });
		}

		// Replies TrainingEnv would have put on the wire. The init reply is the
		// signal that Setup() finished (save loaded, GG_Workshop entered).
		private static void OnOutgoing(Message m)
		{
			string t = m == null ? "(null)" : m.type;
			Log($"env reply: {t}");
			if (t == "init" && !_resetSent)
			{
				_resetSent = true;
				Enqueue("reset", new MessageData { level = _level, frames_per_wait = 5 });
				Log($"enqueued reset level={_level} frames_per_wait=5");
			}
		}

		private static void OnSceneReady(Hooks.SceneContext ctx)
		{
			if (_dumped) { Log("SceneReady again — already dumped, ignoring"); return; }
			_dumped = true;
			string scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
			Log($"SceneReady: activeScene={scene} requested={ctx?.Level} "
				+ $"bossHMs={(ctx?.BossHMs == null ? -1 : ctx.BossHMs.Count)} "
				+ $"timeScale={Time.timeScale} frame={Time.frameCount}");

			string dir = System.IO.Path.Combine(_dumpRoot, scene);
			try { System.IO.Directory.CreateDirectory(dir); }
			catch (Exception e) { Log($"FATAL: cannot create {dir}: {HKOracle.DescribeException(e)}"); }

			var t0 = Time.realtimeSinceStartup;
			try { FsmDumper.Dump(dir, scene, ctx?.Level); }
			catch (Exception e) { Log($"FsmDumper.Dump FAILED: {HKOracle.DescribeException(e)}"); }
			Log($"fsm dump done in {(Time.realtimeSinceStartup - t0) * 1000f:F0}ms");

			t0 = Time.realtimeSinceStartup;
			try { ReflectionDumper.DumpAll(dir, scene, ctx?.Level); }
			catch (Exception e) { Log($"ReflectionDumper.DumpAll FAILED: {HKOracle.DescribeException(e)}"); }
			Log($"reflection dumps done in {(Time.realtimeSinceStartup - t0) * 1000f:F0}ms");

			t0 = Time.realtimeSinceStartup;
			try { SceneDumper.DumpScene(dir, scene); }
			catch (Exception e) { Log($"SceneDumper.DumpScene FAILED: {HKOracle.DescribeException(e)}"); }
			try { HierarchyDumper.Dump(dir, scene); }
			catch (Exception e) { Log($"HierarchyDumper.Dump FAILED: {HKOracle.DescribeException(e)}"); }
			try { SpriteCollectionDumper.Dump(dir, scene); }
			catch (Exception e) { Log($"SpriteCollectionDumper.Dump FAILED: {HKOracle.DescribeException(e)}"); }
			try { SceneDumper.DumpRngProbe(dir); }
			catch (Exception e) { Log($"SceneDumper.DumpRngProbe FAILED: {HKOracle.DescribeException(e)}"); }
			try { LanguageDumper.Dump(dir, scene); }
			catch (Exception e) { Log($"LanguageDumper.Dump FAILED: {HKOracle.DescribeException(e)}"); }
			Log($"scene + rng + lang dumps done in {(Time.realtimeSinceStartup - t0) * 1000f:F0}ms");

			// The engine's native state at this instant (docs/state-record.md "Dump mode").
			if (Mode.Get("HK_ORACLE_DUMP_NATIVE") == "1")
			{
				try { Record.StateRecorder.DumpNative(System.IO.Path.Combine(dir, "native.hkstate")); Log("native.hkstate written"); }
				catch (Exception e) { Log($"StateRecorder.DumpNative FAILED: {HKOracle.DescribeException(e)}"); }
			}

			Log($"TOTALS fsms={FsmDumper.FsmCount} actions={FsmDumper.ActionCount} "
				+ $"actionTypes={FsmDumper.ActionTypes.Count} actionErrors={FsmDumper.ActionErrorCount} "
				+ $"fsmUnserialized={FsmDumper.UnserializedCount} "
				+ $"reflUnserialized={ReflectionDumper.UnserializedCount} "
				+ $"reflErrors={ReflectionDumper.ErrorCount} "
				+ $"langSheets={LanguageDumper.SheetCount} langEntries={LanguageDumper.EntryCount} "
				+ $"spriteCollections={SpriteCollectionDumper.CollectionCount} sprites={SpriteCollectionDumper.SpriteCount}");

			// Provenance: which fight (tier) and capture regime this dump was taken under; a dump
			// from another regime is not comparable to the recordings.
			try
			{
				var bsc = BossSceneController.Instance;
				string E(string k) => System.Environment.GetEnvironmentVariable(k) ?? "";
				var meta = new Newtonsoft.Json.Linq.JObject
				{
					["scene"] = scene, ["levelRequested"] = ctx?.Level ?? _level,
					["tierRequested"] = E("HK_ORACLE_TIER"), ["tierLoaded"] = SceneHooks.LoadedTier,
					["bossLevel"] = bsc == null ? -1 : bsc.BossLevel,
					["bossHMs"] = ctx?.BossHMs == null ? -1 : ctx.BossHMs.Count,
					["modCommit"] = ModInfo.Commit,
					["captureDt"] = Time.captureDeltaTime, ["fixedDt"] = Time.fixedDeltaTime, ["frame"] = Time.frameCount,
					["env"] = new Newtonsoft.Json.Linq.JObject
					{
						["HK_ORACLE_SEED"] = E("HK_ORACLE_SEED"),
					},
					["unityVersion"] = Application.unityVersion, ["timestampUtc"] = DateTime.UtcNow.ToString("o"),
				};
				System.IO.File.WriteAllText(System.IO.Path.Combine(dir, "meta.json"), meta.ToString());
			}
			catch (Exception e) { Log($"meta.json FAILED: {HKOracle.DescribeException(e)}"); }

			Enqueue("close", new MessageData());
			Log("enqueued close; quitting after flush");
			GameManager.instance.StartCoroutine(QuitSoon());
		}

		// Files are written with StreamWriter/using (flushed + closed on return),
		// so this only waits for TrainingEnv to drain the close message.
		private static IEnumerator QuitSoon()
		{
			for (int i = 0; i < 30; i++) yield return null;
			Log("Application.Quit()");
			Quit(0);
		}

		private static void Quit(int code)
		{
			try { Application.Quit(code); }
			catch (Exception e) { Log($"Application.Quit threw: {e.Message}"); }
		}
	}
}
