using System;
using System.Collections.Generic;

namespace HKOracle.Env
{
	// Seam between TrainingEnv and the record/dump features: they subscribe here
	// instead of editing TrainingEnv. Raise* calls sit at the points named in
	// docs/trace-format.md §"Capture points".
	public static class Hooks
	{
		public sealed class SceneContext
		{
			public string Level;
			public HashSet<HealthManager> BossHMs;
			public Game.HitboxObserver Hitboxes;
			public Game.InputDeviceShim Shim;
			public int FramesPerWait;
			public int ResetCount;
		}

		public static SceneContext Current { get; private set; }

		public static event Action<string> ResetBegin;      // requested level (may be null)
		public static event Action<SceneContext> SceneReady; // boss scene loaded, refs bound, world frozen
		public static event Action<int, int[], bool> StepBegin; // step index, action[4], committed
		public static event Action Frame;                    // after each `yield return null` in Step
		public static event Action<string> EpisodeEnd;       // "win" | "loss"
		public static event Action<string, Message, int, int> Obs; // "reset"|"step", wire msg, reset#, step#

		public static void RaiseResetBegin(string level) => ResetBegin?.Invoke(level);
		public static void RaiseSceneReady(SceneContext ctx) { Current = ctx; SceneReady?.Invoke(ctx); }
		public static void RaiseStepBegin(int step, int[] action, bool committed) => StepBegin?.Invoke(step, action, committed);
		public static void RaiseFrame() => Frame?.Invoke();
		public static void RaiseEpisodeEnd(string info) => EpisodeEnd?.Invoke(info);
		public static void RaiseObs(string kind, Message m, int reset, int step) => Obs?.Invoke(kind, m, reset, step);

		// The running env (set by HKOracle.Initialize before Start()). The script and
		// dump drivers feed it via Env.socket.UnreadMessages with Socket.Sink set.
		public static WebsocketEnv Env;
	}
}
