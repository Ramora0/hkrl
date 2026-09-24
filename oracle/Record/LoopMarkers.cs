using System;
using System.Collections.Generic;
using UnityEngine.LowLevel;

namespace HKOracle.Record
{
	// Inserts a marker system before every native PlayerLoop subsystem and at the end of every group, so any
	// callback can be attributed to the native stage it runs in (docs/engine-lifecycle.md §1 "Stage names").
	// Used by LifecycleRecorder (records every marker) and the conformance probes (oracle/Probe/ProbeStages.cs).
	public static class LoopMarkers
	{
		// Returns the marker names by id ("EarlyUpdate/ScriptRunDelayedStartupFrame", "FixedUpdate/<end>", ...).
		// `markerType` tags the inserted systems; `onMark(id)` runs when the loop reaches marker `id`.
		public static List<string> Install(Type markerType, Action<int> onMark)
		{
			var names = new List<string>();
			var root = PlayerLoop.GetCurrentPlayerLoop();
			root = Wrap(root, "", markerType, onMark, names);
			PlayerLoop.SetPlayerLoop(root);
			return names;
		}

		private static PlayerLoopSystem Wrap(PlayerLoopSystem sys, string prefix, Type markerType, Action<int> onMark,
			List<string> names)
		{
			if (sys.subSystemList == null || sys.subSystemList.Length == 0) return sys;
			var list = new List<PlayerLoopSystem>(sys.subSystemList.Length * 2 + 1);
			foreach (var sub in sys.subSystemList)
			{
				string nm = (prefix.Length > 0 ? prefix + "/" : "") + (sub.type != null ? sub.type.Name : "?");
				int id = names.Count; names.Add(nm);
				list.Add(new PlayerLoopSystem { type = markerType, updateDelegate = () => onMark(id) });
				list.Add(Wrap(sub, nm, markerType, onMark, names));
			}
			int endId = names.Count; names.Add((prefix.Length > 0 ? prefix : "<root>") + "/<end>");
			list.Add(new PlayerLoopSystem { type = markerType, updateDelegate = () => onMark(endId) });
			sys.subSystemList = list.ToArray();
			return sys;
		}
	}
}
