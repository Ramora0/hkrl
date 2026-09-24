using System.Collections.Generic;
using HKOracle.Record;

namespace HKOracle.Probe
{
	// The player-loop stage the main thread is in, from LifecycleRecorder's marker systems (LoopMarkers).  Stage
	// names are docs/engine-lifecycle.md §1's; any other native subsystem is "other:<name>".
	public static class ProbeStages
	{
		private sealed class ProbeLoopMarker { }

		private static readonly Dictionary<string, string> kStageOf = new Dictionary<string, string>
		{
			{ "EarlyUpdate/UpdatePreloading", "load" },
			{ "EarlyUpdate/ScriptRunDelayedStartupFrame", "startup" },
			{ "FixedUpdate/ScriptRunBehaviourFixedUpdate", "fixed" },
			{ "FixedUpdate/Physics2DFixedUpdate", "physics" },
			{ "FixedUpdate/ScriptRunDelayedFixedFrameRate", "fixed_delayed" },
			{ "Update/ScriptRunBehaviourUpdate", "update" },
			{ "Update/ScriptRunDelayedDynamicFrameRate", "update_delayed" },
			{ "PreLateUpdate/ScriptRunBehaviourLateUpdate", "late" },
			{ "PostLateUpdate/ScriptRunDelayedDynamicFrameRate", "postlate_delayed" },
			// WaitForEndOfFrame resumes follow this marker (tools/lifecycle_rules.py coroutine_resume)
			{ "PostLateUpdate/PlayerSendFrameComplete", "end_of_frame" },
		};

		private static string[] _stage = new string[0];
		public static string Current { get; private set; } = "none";

		public static void Install()
		{
			var names = LoopMarkers.Install(typeof(ProbeLoopMarker), id => Current = _stage[id]);
			var st = new string[names.Count];
			for (int i = 0; i < names.Count; i++) st[i] = kStageOf.TryGetValue(names[i], out var s) ? s : "other:" + names[i];
			_stage = st;
		}
	}
}
