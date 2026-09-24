using HKOracle.Dump;
using HKOracle.Env;
using HKOracle.Probe;
using HKOracle.Record;

namespace HKOracle
{
	// How this process was launched, read once from the environment (docs/oracle.md).
	//   eval   (nothing set): TrainingEnv serves the trainer over the WebSocket; nothing is recorded.
	//   record (HK_ORACLE_TRACE): the recorders write the trace and its side files next to it.
	//   script (HK_ORACLE_SCRIPT): a corpus drives TrainingEnv instead of a server; used with record.
	//   dump   (HK_ORACLE_DUMPS, no script): dump the scene at SceneReady and quit.
	//   probe  (HK_ORACLE_PROBE): run the engine conformance scenarios at SceneReady and quit (oracle/Probe).
	public static class Mode
	{
		public static readonly string TracePath = Get("HK_ORACLE_TRACE");
		public static readonly string ScriptPath = Get("HK_ORACLE_SCRIPT");
		public static readonly string DumpRoot = Get("HK_ORACLE_DUMPS");
		public static readonly string ProbeName = Get("HK_ORACLE_PROBE");

		public static bool Record => TracePath != null;
		public static bool Script => ScriptPath != null;
		public static bool Dump => DumpRoot != null && !Script;
		public static bool Probe => ProbeName != null;

		// <trace base><ext>, where the base is HK_ORACLE_TRACE without ".hktrace".
		public static string SidePath(string ext)
		{
			string b = TracePath;
			if (b.EndsWith(".hktrace", System.StringComparison.OrdinalIgnoreCase))
				b = b.Substring(0, b.Length - ".hktrace".Length);
			return b + ext;
		}

		public static string Get(string name)
		{
			string v = System.Environment.GetEnvironmentVariable(name);
			return string.IsNullOrEmpty(v) ? null : v;
		}

		// Called once from HKOracle.Initialize, after Hooks.Env is set and before the env starts.
		public static void Install()
		{
			HKOracle.Instance.Log($"[Oracle] mode: record={Record} script={Script} dump={Dump} probe={Probe}");
			if (Probe) Run("ProbeDriver", ProbeDriver.Install);
			else if (Dump) Run("DumpDriver", DumpDriver.Install);
			if (Record) Run("FsmTickRecorder", FsmTickRecorder.Install);
			if (Record && Get("HK_ORACLE_LIFECYCLE") == "1") Run("LifecycleRecorder", LifecycleRecorder.Install);
			if (Record && Get("HK_ORACLE_METHODS") == "1") Run("MethodRecorder", MethodRecorder.Install);
			Run("RegimeTweaks", RegimeTweaks.Install);
			if (Record) Run("RngDrawRecorder", RngDrawRecorder.Install);
			if (Record && Get("HK_ORACLE_WALLCLOCK") == "1") Run("WallClockCounter", WallClockCounter.Install);
			if (Script) Run("ScriptDriver", ScriptDriver.Install);
			if (Record) Run("TraceRecorder", TraceRecorder.Install);
			if (Record && Get("HK_ORACLE_STATE") == "1") Run("StateRecorder", StateRecorder.Install);
		}

		private static void Run(string name, System.Action install)
		{
			try { install(); HKOracle.Instance.Log($"[Oracle] installed {name}"); }
			catch (System.Exception e) { HKOracle.Instance.Log($"[Oracle] {name}.Install threw: {HKOracle.DescribeException(e)}"); }
		}
	}
}
