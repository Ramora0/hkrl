using System.IO;
using HKOracle.Record;

// The known answers are asserted in tests/test_staterec.py; change both together.  With a second argument
// "variant", A.n is 5 instead of 4 from frame 1 on (the record tools/staterec_check.py --compare must reject).
// "clockA" / "clockB" write one short episode as two game processes would (other frame counts, clocks, instance
// ids and counters), which --compare must accept; "clockC" is clockB with one real divergence, which it must reject.
static class Program
{
	static int Main(string[] args)
	{
		string mode = args.Length > 1 ? args[1] : "";
		if (mode.StartsWith("clock")) return Clock(args[0], mode);
		if (mode == "slots") return Slots(args[0]);
		int n1 = args.Length > 1 && args[1] == "variant" ? 5 : 4;
		var w = new StateWriter(File.Create(args[0]), "{\"fixture\":\"known\",\"level\":\"TEST\"}");
		var obj = w.Class("Obj");
		var bag = w.Class("Bag");
		var nat = w.Class("Native");
		object plain = new object();
		long keyA = StateWriter.Key(1, 42), keyC = StateWriter.Key(2, 0x1000);

		// frame 0: every scalar and list type, a note
		w.BeginFrame(100, -1, 7, 1);
		var a = w.Visit(keyA, obj, 0, 0, "A");
		w.F(a, "x", 1.5f); w.I(a, "n", -3); w.B(a, "ok", true); w.S(a, "name", "alpha");
		w.O(a, "ref", -12345); w.L(a, "big", -1234567890123L); w.D(a, "t", 0.02f); w.E(a, "self", a.Eid);
		w.Commit(a);
		var b = w.Visit(plain, bag, a.Eid, "B");
		w.List(b, "fl", 'F', new[] { StateWriter.FloatBits(0.25f), StateWriter.FloatBits(-2f) });
		w.List(b, "il", 'I', new long[] { 1, -2, 300000 });
		w.Strs(b, "sl", new[] { "p", "", "q" });
		w.List(b, "ol", 'O', new long[] { -5, 7 });
		w.List(b, "el", 'E', new long[] { a.Eid });
		w.List(b, "ll", 'L', new long[] { long.MinValue, 5 });
		w.Commit(b);
		w.Note("hello");
		w.EndFrame();

		// frame 1: A changes one field and gains a field; B dies; C is born
		w.BeginFrame(101, 0, 8, 0);
		a = w.Visit(keyA, obj, 0, 0, "A");
		w.F(a, "x", 1.5f); w.I(a, "n", n1); w.I(a, "extra", 9);
		w.Commit(a);
		var c = w.Visit(keyC, nat, 1, 0, "C");
		w.I(c, "v", 1);
		w.Commit(c);
		w.EndFrame();

		// frame 2: nothing on A changes; C's key comes back with another signature: C dies, D is born
		w.BeginFrame(102, 0, 9, 1);
		a = w.Visit(keyA, obj, 0, 0, "A");
		w.F(a, "x", 1.5f); w.I(a, "n", n1);
		w.Commit(a);
		var d = w.Visit(keyC, nat, 2, 0, "D");
		w.I(d, "v", 1);
		w.Commit(d);
		w.EndFrame();

		w.Close("{\"frames\":3}");
		return 0;
	}

	// Frames F0..F0+2 of one episode.  Fields tied to the process: the frame counters, Time.time/fixedTime and the
	// double clocks (T0 + 0.02 k), values derived from them (a MonoBehaviour's Time.time + 0.25, a delayed call's due
	// time and frame), instance ids (in iid, parent, o fields, keys and formatted text), an asset id and a counter.
	// The recorder's fast path: puts by slot, a list from a scratch buffer, an unchanged string reference not
	// re-put, a class with no fields, and the background writer thread.
	static int Slots(string path)
	{
		var w = new StateWriter(File.Create(path), "{\"fixture\":\"slots\"}", async: true);
		var obj = w.Class("Obj");
		var empty = w.Class("Empty");
		int sx = w.Field(obj, "x", 'f'), sl = w.Field(obj, "l", 'I'), ss = w.Field(obj, "s", 's');
		var buf = new long[] { 7, -8, 9 };
		string one = "one";
		for (int f = 0; f < 3; f++)
		{
			w.BeginFrame(10 + f, f - 1, f, 1);
			var a = w.Visit(StateWriter.Key(1, 1), obj, 0, 0, "A");
			w.Raw(a, sx, StateWriter.FloatBits(f == 0 ? 2.5f : 3.5f));
			w.RawList(a, sl, buf, f < 2 ? 2 : 1);
			if (!w.SameRef(a, ss, one)) { w.Raw(a, ss, w.Str(one)); w.Ref(a, ss, one); }
			w.Commit(a);
			var z = w.Visit(StateWriter.Key(1, 2), empty, 0, a.Eid, "Z");
			w.Commit(z);
			w.EndFrame();
		}
		w.Close("{\"frames\":3}");
		return 0;
	}

	static int Clock(string path, string mode)
	{
		bool b = mode != "clockA";
		int f0 = b ? 27060 : 24572, fix0 = b ? 1300 : 1000, iidBase = b ? -7300 : -5000, asset = b ? 12000 : 9000, stamp0 = b ? 350 : 100;
		double t0 = b ? 22.87 : 20.90;
		var w = new StateWriter(File.Create(path), "{\"fixture\":\"" + mode + "\"}");
		var time = w.Class("Time"); var tm = w.Class("TimeManager"); var go = w.Class("GameObject");
		var mono = w.Class("Mono"); var call = w.Class("DelayedCall"); var body = w.Class("b2Body");
		int goIid = iidBase + 10, compIid = iidBase + 11;
		for (int k = 0; k < 3; k++)
		{
			double t = t0 + 0.02 * k;
			w.BeginFrame(f0 + k, k - 1, fix0 + k, 1);
			var te = w.Visit(StateWriter.Key(9, 1001), time, 0, 0, "Time");
			w.I(te, "frameCount", f0 + k); w.F(te, "time", (float)t); w.F(te, "fixedTime", (float)t); w.F(te, "deltaTime", 0.02f);
			w.Commit(te);
			var me = w.Visit(StateWriter.Key(9, 1), tm, 0, 0, "TimeManager");
			w.D(me, "active.cur", t); w.D(me, "fixed.cur", t); w.L(me, "frameCount", f0 + k);
			w.Commit(me);
			var g = w.Visit(StateWriter.Key(1, goIid), go, 0, 0, "Root");
			if (g.IsNew) w.O(g, "iid", goIid);
			w.O(g, "parent", 0); w.List(g, "components", 'O', new long[] { compIid });
			w.Commit(g);
			var m = w.Visit(StateWriter.Key(1, compIid), mono, 0, g.Eid, "Mono");
			if (m.IsNew) w.O(m, "iid", compIid);
			w.F(m, "nextTinkTime", (float)t + 0.25f);
			w.F(m, "timer", mode == "clockC" && k == 2 ? 0.52f : 0.5f);
			w.O(m, "clip", asset);
			w.S(m, "onDone", "Tk2dPlayAnimationWithEvents.AnimationCompleted@#" + goIid + "/Idle/0");
			w.Commit(m);
			var c = w.Visit(StateWriter.Key(8, 0x5000 + (b ? 0x777 : 0)), call, 0, 0, "Coroutine::ContinueCoroutine");
			w.D(c, "time", t + 0.1); w.L(c, "frame", f0 + k + 3); w.O(c, "object", compIid); w.I(c, "timeStamp", stamp0 + k);
			w.Commit(c);
			var bo = w.Visit(StateWriter.Key(2, 0x9000 + (b ? 0x40 : 0)), body, 0, 0, "rb#" + compIid);
			w.O(bo, "rigidbody", compIid);
			w.Commit(bo);
			w.EndFrame();
		}
		w.Close("{\"frames\":3}");
		return 0;
	}
}
