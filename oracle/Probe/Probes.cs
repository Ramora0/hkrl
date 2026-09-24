using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;

namespace HKOracle.Probe
{
	// The conformance probes: MonoBehaviours that log every Unity message they receive (Scenario.OnCallback).
	// Unity dispatches a message only to a type that declares it, so each kind is its own class declaring
	// exactly its set; the base declares none.  hkpy/conformance.py KINDS mirrors this table.
	public abstract class ProbeBase : MonoBehaviour
	{
		// Not serialized, so Object.Instantiate does not copy it: a clone's label is derived from its name until
		// the instantiate op renames it (Scenario.LabelOf).
		[NonSerialized] public string Label;
		[NonSerialized] public string Kind;
		// The point overlap-tested (Physics2D.OverlapPoint, scenario layer) inside Awake/OnEnable/OnDisable/
		// OnDestroy, relative to the scenario origin: collider membership at those moments (A-6).
		[NonSerialized] public Vector2? Query;

		protected void L(string cb) => Scenario.Current?.OnCallback(this, cb, null);
		protected void LT(string cb, Collider2D other) => Scenario.Current?.OnCallback(this, cb, other == null ? null : other.gameObject);
		protected void LC(string cb, Collision2D c) => Scenario.Current?.OnCallback(this, cb, c == null || c.collider == null ? null : c.collider.gameObject);

		// A scripted coroutine: logs "co:<name>" with its step index k at the first MoveNext (k = 0, inside the
		// StartCoroutine caller) and after each yield.  Yield codes: "null", "fixed" (WaitForFixedUpdate), "eof"
		// (WaitForEndOfFrame), a number (WaitForSeconds).
		public IEnumerator Scripted(string name, IList<object> yields)
		{
			string cb = "co:" + name;
			for (int k = 0; ; k++)
			{
				Scenario.Current?.OnCallback(this, cb, null, k);
				if (k >= yields.Count) yield break;
				object y = yields[k];
				if (y is string s)
				{
					if (s == "null") yield return null;
					else if (s == "fixed") yield return new WaitForFixedUpdate();
					else if (s == "eof") yield return new WaitForEndOfFrame();
					else throw new ArgumentException("unknown yield " + s);
				}
				else yield return new WaitForSeconds(Convert.ToSingle(y));
			}
		}
	}

	public class ProbeFull : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void Start() => L("Start");
		void FixedUpdate() => L("FixedUpdate");
		void Update() => L("Update");
		void LateUpdate() => L("LateUpdate");
		void OnDisable() => L("OnDisable");
		void OnDestroy() => L("OnDestroy");
		void OnTriggerEnter2D(Collider2D o) => LT("OnTriggerEnter2D", o);
		void OnTriggerStay2D(Collider2D o) => LT("OnTriggerStay2D", o);
		void OnTriggerExit2D(Collider2D o) => LT("OnTriggerExit2D", o);
		void OnCollisionEnter2D(Collision2D c) => LC("OnCollisionEnter2D", c);
		void OnCollisionStay2D(Collision2D c) => LC("OnCollisionStay2D", c);
		void OnCollisionExit2D(Collision2D c) => LC("OnCollisionExit2D", c);
	}

	public class ProbeLife : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void Start() => L("Start");
		void OnDisable() => L("OnDisable");
		void OnDestroy() => L("OnDestroy");
	}

	public class ProbeUpd : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void Start() => L("Start");
		void Update() => L("Update");
		void OnDisable() => L("OnDisable");
	}

	public class ProbeLate : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void Start() => L("Start");
		void LateUpdate() => L("LateUpdate");
		void OnDisable() => L("OnDisable");
	}

	public class ProbeFix : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void Start() => L("Start");
		void FixedUpdate() => L("FixedUpdate");
		void OnDisable() => L("OnDisable");
	}

	// No Awake / OnEnable / OnDisable: the invisible enables of A-15.
	public class ProbeTicks : ProbeBase
	{
		void Start() => L("Start");
		void FixedUpdate() => L("FixedUpdate");
		void Update() => L("Update");
		void LateUpdate() => L("LateUpdate");
	}

	public class ProbeNoStart : ProbeBase
	{
		void Awake() => L("Awake");
		void OnEnable() => L("OnEnable");
		void FixedUpdate() => L("FixedUpdate");
		void Update() => L("Update");
		void LateUpdate() => L("LateUpdate");
		void OnDisable() => L("OnDisable");
	}

	public class ProbePhys : ProbeBase
	{
		void OnEnable() => L("OnEnable");
		void OnDisable() => L("OnDisable");
		void OnTriggerEnter2D(Collider2D o) => LT("OnTriggerEnter2D", o);
		void OnTriggerStay2D(Collider2D o) => LT("OnTriggerStay2D", o);
		void OnTriggerExit2D(Collider2D o) => LT("OnTriggerExit2D", o);
		void OnCollisionEnter2D(Collision2D c) => LC("OnCollisionEnter2D", c);
		void OnCollisionStay2D(Collision2D c) => LC("OnCollisionStay2D", c);
		void OnCollisionExit2D(Collision2D c) => LC("OnCollisionExit2D", c);
	}

	public static class ProbeKinds
	{
		public static readonly Dictionary<string, Type> Types = new Dictionary<string, Type>
		{
			{ "full", typeof(ProbeFull) }, { "life", typeof(ProbeLife) }, { "upd", typeof(ProbeUpd) },
			{ "late", typeof(ProbeLate) }, { "fix", typeof(ProbeFix) }, { "ticks", typeof(ProbeTicks) },
			{ "nostart", typeof(ProbeNoStart) }, { "phys", typeof(ProbePhys) },
		};
	}

	// The per-scenario driver: runs the scenario's "at" triggers from its own callbacks, one hook per stage
	// (fixed, physics via a permanent trigger overlap, fixed_delayed, update, update_delayed, late,
	// end_of_frame), and sets Time.timeScale for the next frame from the scenario's frame shape.
	public class ProbeDriverBehaviour : MonoBehaviour
	{
		[NonSerialized] public Scenario Sc;
		private int _fixedThisFrame, _fixedFrame = -1;

		public void StartLoops()
		{
			StartCoroutine(Loop("update_delayed", null));
			StartCoroutine(Loop("fixed_delayed", new WaitForFixedUpdate()));
			StartCoroutine(Loop("end_of_frame", new WaitForEndOfFrame()));
		}

		private IEnumerator Loop(string stage, object y)
		{
			for (;;)
			{
				yield return y;
				Sc?.At(stage);
			}
		}

		void FixedUpdate()
		{
			if (_fixedFrame != Time.frameCount) { _fixedFrame = Time.frameCount; _fixedThisFrame = 0; }
			_fixedThisFrame++;
			Sc?.At("fixed");
		}
		void OnTriggerEnter2D(Collider2D o) => Sc?.At("physics");
		void OnTriggerStay2D(Collider2D o) => Sc?.At("physics");
		void Update() => Sc?.At("update");
		void LateUpdate()
		{
			Sc?.At("late");
			Sc?.EndOfLate(_fixedFrame == Time.frameCount ? _fixedThisFrame : 0);
		}
	}
}
