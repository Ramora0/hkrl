using Modding;

namespace HKOracle
{
	internal class HKOracle : Mod
	{
		internal static HKOracle Instance { get; private set; }

		// The trainer's WebSocket server; FK_SERVER_URL overrides (one port per fleet).
		private string _serverUrl =
			System.Environment.GetEnvironmentVariable("FK_SERVER_URL")
			?? "ws://localhost:8765";

		// Per-instance log mirror: all instances share one LocalLow, so ModLog.txt
		// keeps only one of them. Every mod log line also goes to
		// HKOracle_<exe name>.log, append-mode with a boot header.
		private static System.IO.StreamWriter _mirror;
		private static MonoMod.RuntimeDetour.Hook _bossLevelHook;

		private static void InitLogMirror()
		{
			try
			{
				string name = System.Diagnostics.Process
					.GetCurrentProcess().ProcessName;
				int pid = System.Diagnostics.Process.GetCurrentProcess().Id;
				string path = System.IO.Path.Combine(
					UnityEngine.Application.persistentDataPath,
					$"HKOracle_{name}.log");
				_mirror = new System.IO.StreamWriter(path, append: true);
				_mirror.AutoFlush = true;
				_mirror.WriteLine(
					$"===== boot {System.DateTime.Now:yyyy-MM-dd HH:mm:ss} "
					+ $"pid={pid} =====");
			}
			catch { _mirror = null; }
		}

		public new void Log(object message)
		{
			base.Log(message);
			try
			{
				_mirror?.WriteLine(
					$"[{System.DateTime.Now:HH:mm:ss.fff}] {message}");
			}
			catch { }
		}

		// An exception with its full inner chain (a TypeInitializationException's
		// cause is only in InnerException).
		internal static string DescribeException(System.Exception e)
		{
			var sb = new System.Text.StringBuilder();
			int depth = 0;
			while (e != null && depth < 8)
			{
				if (depth > 0) sb.Append(" <== inner: ");
				sb.Append(e.GetType().Name).Append(": ").Append(e.Message);
				if (depth == 0 && e.StackTrace != null)
					sb.Append(" @ ").Append(
						e.StackTrace.Replace("\n", " | ").Replace("\r", ""));
				e = e.InnerException;
				depth++;
			}
			return sb.ToString();
		}

		public override void Initialize()
		{
			Instance = this;
			InitLogMirror();
			Log($"HKOracle initializing (server={_serverUrl})");

			// Boot-time Language probe. Language.Language's static cctor reads the
			// shared LocalLow tree and can fail when many instances boot at once,
			// poisoning the type for the process. Probe now so the cause is logged.
			try
			{
				string probe = global::Language.Language.Get("MAIN", "MainMenu");
				Log($"[LangProbe] ok (probe='{probe}')");
			}
			catch (System.Exception e)
			{
				Log($"[LangProbe] POISONED at boot: {DescribeException(e)}");
			}

			// Survive the poison: a throw from BossChallengeUI.Setup's Language.Get
			// propagates through Fsm.SwitchState, drops the DREAM event and parks the
			// bossUI FSM at Open UI. Setup assigns bossStatue (the only load-critical
			// field) before that call; the rest is panel cosmetics.
			On.BossChallengeUI.Setup += (orig, self, statue, nameSheet,
				nameKey, descSheet, descKey) =>
			{
				try
				{
					orig(self, statue, nameSheet, nameKey, descSheet, descKey);
				}
				catch (System.Exception e)
				{
					Log("[LangGuard] BossChallengeUI.Setup threw (cosmetic "
						+ $"only, continuing): {DescribeException(e)}");
				}
			};

			// Tier clamp: while SceneHooks has a canonical load in flight
			// (ForcedTier >= 0), every LoadBoss call, ours or a stray UI submit, is
			// forced to the requested tier. At BossLevel 2 all damage to the knight
			// becomes 9999 (HeroController.TakeDamage).
			On.BossChallengeUI.LoadBoss_int_bool += (orig, self, level, doHideAnim) =>
			{
				int forced = Game.SceneHooks.ForcedTier;
				if (forced >= 0 && level != forced)
				{
					Log($"[TierGuard] LoadBoss(level={level}) overridden -> {forced}");
					level = forced;
				}
				orig(self, level, doHideAnim);
			};
			On.BossChallengeUI.LoadBoss_int += (orig, self, level) =>
			{
				int forced = Game.SceneHooks.ForcedTier;
				if (forced >= 0 && level != forced)
				{
					Log($"[TierGuard] LoadBoss(level={level}) overridden -> {forced}");
					level = forced;
				}
				orig(self, level);
			};

			// Tier invariant at the BossLevel setter: scene-side code (a PlayMaker
			// SetProperty on GG_Broken_Vessel) rewrites BossLevel mid-episode. Every
			// write other than the loaded tier is suppressed and its stack logged.
			// HookGen has no On. entry for property accessors, so detour by hand.
			var bossLevelSetter = typeof(BossSceneController)
				.GetProperty("BossLevel")?.GetSetMethod();
			if (bossLevelSetter != null)
			{
				_bossLevelHook = new MonoMod.RuntimeDetour.Hook(
					bossLevelSetter,
					new System.Action<System.Action<BossSceneController, int>,
						BossSceneController, int>((orig, self, value) =>
					{
						// The tier the canonical load selected (HK_ORACLE_TIER, or the statue
						// slot that owns this scene); `_V` scenes exist only at tier 1.
						// HealthManager.Awake scales HP off this value.
						int want = Game.SceneHooks.ForcedTier >= 0 ? Game.SceneHooks.ForcedTier
							: (Game.SceneHooks.LoadedTier >= 0 ? Game.SceneHooks.LoadedTier : 0);
						if (value != want)
						{
							Log($"[TierTrace] set_BossLevel({value}) suppressed"
								+ $" -> {want}; stack: {System.Environment.StackTrace}");
							value = want;
						}
						orig(self, value);
					}));
			}
			else
			{
				Log("[TierTrace] WARNING: BossLevel setter not found; "
					+ "no root tier enforcement");
			}

			var env = new Env.TrainingEnv(_serverUrl);
			Env.Hooks.Env = env;
			Mode.Install();
			// Fail closed: an instance outside regime R2 does not serve (oracle/Env/RegimeTweaks.cs).
			if (Env.RegimeTweaks.Failure != null)
				Log("[Oracle] NOT starting the env: regime R2 is not installed: " + Env.RegimeTweaks.Failure);
			else
				env.Start();
		}

		public override string GetVersion() => "1.0.0";
	}
}
